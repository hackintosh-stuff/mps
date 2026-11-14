#!/usr/bin/env python3
"""Native MPS IOUserClient client. --encode-only never connects to hardware.

All mutating device commands additionally require the driver's boot-time
mps_maintenance=1 mode, which suppresses block-media publication. The tool never
changes boot arguments, mounts a volume, deploys a kext, or reboots the machine.
"""
from __future__ import annotations
import argparse
import ctypes as C
import json
import struct
import sys
from pathlib import Path
from typing import Any

VERSION, CONNECT_TYPE, MAX_DATA = 1, 0x4D505301, 16 * 1024 * 1024
INPUT = struct.Struct('<8I2Q4I')
OUTPUT = struct.Struct('<8IQ6I')
OUTPUT_BYTES = 416
OPERATIONS = {name: i + 1 for i, name in enumerate((
    'info', 'targets', 'events', 'event-mask', 'passthrough', 'reset', 'rescan',
    'register-read', 'register-write', 'diag-register', 'diag-release',
    'diag-query', 'diag-read', 'diag-unregister'))}
FUNCTIONS = {'scsi': 0x00, 'task-management': 0x01, 'ioc-facts': 0x03,
    'config': 0x04, 'port-facts': 0x05, 'fw-download': 0x09, 'fw-upload': 0x12,
    'raid-action': 0x15, 'raid-scsi': 0x16, 'toolbox-cli': 0x17, 'sep': 0x18,
    'smp': 0x1A, 'sas-control': 0x1B, 'sata': 0x1C}


def number(value: Any, bits: int = 32) -> int:
    if isinstance(value, bool) or not isinstance(value, (int, str)):
        raise ValueError('integer parameters must be integers or numeric strings')
    result = int(value, 0) if isinstance(value, str) else int(value)
    if result < 0 or result >= 1 << bits:
        raise ValueError(f'integer outside unsigned {bits}-bit range: {value}')
    return result


def lun_bytes(lun: int) -> bytes:
    lun = number(lun, 24)
    if lun <= 255:
        return bytes((0, lun)) + bytes(6)
    if lun <= 0x3FFF:
        return bytes((0x40 | (lun >> 8), lun & 255)) + bytes(6)
    return b'\xd2' + lun.to_bytes(3, 'big') + bytes(4)


def blob(spec: dict[str, Any], hex_key: str, file_key: str, base: Path) -> bytes:
    if hex_key in spec and file_key in spec:
        raise ValueError(f'{hex_key} and {file_key} are mutually exclusive')
    if file_key in spec:
        path = base / str(spec[file_key])
        if path.stat().st_size > MAX_DATA:
            raise ValueError(f'{file_key} exceeds 16 MiB')
        return path.read_bytes()
    return bytes.fromhex(str(spec.get(hex_key, '')))


def prefix(spec: dict[str, Any], payload: bytes, read_bytes: int) -> bytes:
    if 'requestHex' in spec:
        if 'function' in spec:
            raise ValueError('use function fields or requestHex, not both')
        raw = bytes.fromhex(str(spec['requestHex']))
        if len(raw) < 12 or raw[3] not in FUNCTIONS.values():
            raise ValueError('raw request uses an unsupported SAS2 MPI function')
        return raw
    name = spec.get('function')
    if not name:
        return b''
    if name not in FUNCTIONS:
        raise ValueError(f'unknown MPI function: {name}')
    sizes = {'scsi': 96, 'raid-scsi': 96, 'ioc-facts': 12, 'port-facts': 12,
        'config': 28, 'fw-download': 36, 'fw-upload': 36, 'raid-action': 20,
        'sep': 32, 'smp': 32, 'sata': 48, 'toolbox-cli': 112,
        'sas-control': 44, 'task-management': 52}
    frame = bytearray(sizes[name])
    frame[3] = FUNCTIONS[name]
    u = lambda key, default=0, bits=32: number(spec.get(key, default), bits)
    if name in ('scsi', 'raid-scsi', 'sata', 'task-management', 'sep'):
        struct.pack_into('<H', frame, 0, u('handle', bits=16))
    if name in ('scsi', 'raid-scsi'):
        cdb = bytes.fromhex(str(spec['cdbHex']))
        if not 1 <= len(cdb) <= 32:
            raise ValueError('CDB must contain 1..32 bytes')
        struct.pack_into('<H', frame, 36, len(cdb))
        frame[52:60] = lun_bytes(u('lun', bits=24))
        frame[64:64 + len(cdb)] = cdb
    elif name == 'config':
        frame[0] = u('action', bits=8)
        page_type = u('pageType', bits=4)
        frame[20] = u('pageVersion', bits=8)
        frame[22] = u('pageNumber', bits=8)
        frame[23] = page_type
        if page_type == 15:
            struct.pack_into('<H', frame, 4, u('pageWords', bits=16))
            frame[6] = u('extendedPageType', bits=8)
            if not 0x10 <= frame[6] <= 0x1A:
                raise ValueError('extended config page type is outside the SAS2 reference')
        else:
            frame[21] = u('pageWords', bits=8)
        struct.pack_into('<I', frame, 24, u('pageAddress'))
    elif name in ('fw-download', 'fw-upload'):
        frame[0] = u('imageType', bits=8)
        if name == 'fw-download':
            last = spec.get('lastSegment', True)
            if not isinstance(last, bool):
                raise ValueError('lastSegment must be a boolean')
            frame[7] = int(last)
            total, offset = u('totalImageBytes', len(payload)), u('imageOffset')
            if not payload or offset >= total or len(payload) > total - offset or last != (offset + len(payload) == total):
                raise ValueError('firmware segment bounds/lastSegment mismatch')
            struct.pack_into('<I', frame, 12, total)
        frame[22] = 12  # TransactionContext: ContextSize=0, DetailsLength=12.
        struct.pack_into('<II', frame, 28, u('imageOffset'), len(payload) if payload else read_bytes)
    elif name == 'raid-action':
        frame[0] = u('action', bits=8)
        struct.pack_into('<H', frame, 4, u('volumeHandle', bits=16))
        frame[6] = u('physicalDisk', bits=8)
        struct.pack_into('<I', frame, 16, u('actionWord'))
    elif name == 'smp':
        frame[1] = u('physicalPort', 255, 8)
        struct.pack_into('<Q', frame, 16, u('sasAddress', bits=64))
    elif name == 'sep':
        frame[4] = u('action', 1, 8)
        frame[5] = u('addressBySlot', 0, 1)
        struct.pack_into('<I', frame, 12, u('slotStatus'))
        struct.pack_into('<HH', frame, 28, u('slot', bits=16), u('enclosureHandle', bits=16))
    elif name == 'sata':
        fis = bytes.fromhex(str(spec['fisHex']))
        if len(fis) != 20:
            raise ValueError('SATA command FIS must contain 20 bytes')
        struct.pack_into('<H', frame, 4, u('passThroughFlags', 0x10, 16))
        frame[28:48] = fis
    elif name == 'port-facts':
        frame[6] = u('port', bits=8)
    elif name == 'toolbox-cli':
        frame[0] = 0x06
        command = bytes.fromhex(str(spec['commandHex']))
        if len(command) > 92:
            raise ValueError('diagnostic CLI command exceeds 92 bytes')
        frame[20:20 + len(command)] = command
    elif name == 'sas-control':
        frame[0] = u('action', bits=8)
        struct.pack_into('<H', frame, 4, u('handle', bits=16))
        frame[14] = u('phy', bits=8)
        frame[15] = u('primitiveFlags', bits=8)
        struct.pack_into('<I', frame, 16, u('primitive'))
        frame[20] = u('lookupMethod', bits=8)
        struct.pack_into('<H', frame, 22, u('slot', bits=16))
        struct.pack_into('<Q', frame, 24, u('lookupAddress', bits=64))
    elif name == 'task-management':
        frame[5] = u('taskType', 5, 8)
        frame[12:20] = lun_bytes(u('lun', bits=24))
    return bytes(frame)


def encode(spec: dict[str, Any], *, base: Path = Path('.'), allow_mutation: bool = False) -> bytes:
    op_name = spec.get('operation', 'passthrough')
    if op_name not in OPERATIONS:
        raise ValueError(f'unknown operation: {op_name}')
    defaults = {'info': 448, 'targets': 64 * 256, 'events': 128 * 160, 'diag-query': 32,
                'register-read': 4, 'register-write': 4}
    incoming = number(spec.get('readBytes', defaults.get(op_name, 0)))
    payload = blob(spec, 'dataOutHex', 'dataOutFile', base)
    request = prefix(spec, payload, incoming)
    if op_name == 'passthrough' and max(len(payload), incoming) > 0xFFFFFF:
        raise ValueError('a legacy MPI SGE can encode at most 0xFFFFFF bytes')
    if op_name == 'event-mask':
        masks = [0] * 4
        for event in spec.get('events', []):
            event = number(event, 7)
            masks[event // 32] |= 1 << (event % 32)
        request = struct.pack('<4I', *masks)
    elif op_name == 'diag-register':
        request = bytearray(100)
        struct.pack_into('<I', request, 0, number(spec['uniqueId']))
        request[4] = number(spec.get('extendedType', 0), 8)
        words = spec.get('productSpecific', [])
        if len(words) > 23:
            raise ValueError('at most 23 diagnostic product-specific words')
        for i, value in enumerate(words):
            struct.pack_into('<I', request, 8 + i * 4, number(value))
        request = bytes(request)
    if len(request) > 256 or len(payload) > MAX_DATA or incoming > MAX_DATA:
        raise ValueError('request or transfer exceeds the native ABI limit')
    timeout = number(spec.get('timeoutMs', 0))
    if timeout and not 100 <= timeout <= 300000:
        raise ValueError('timeoutMs must be 0 or 100..300000')
    arg0 = number(spec.get('argument0', spec.get('wwid', spec.get('bufferType', spec.get('offset', 0)))), 64)
    arg1 = number(spec.get('argument1', spec.get('downloadToken', spec.get('bufferBytes', spec.get('value', 0)))), 64)
    if spec.get('function') == 'fw-download' and not arg1:
        raise ValueError('firmware download requires a nonzero downloadToken shared by all segments')
    generation = number(spec.get('generation', spec.get('uniqueId', 0)))
    header = INPUT.pack(VERSION, OPERATIONS[op_name], int(allow_mutation), timeout,
        len(request), len(payload), incoming, 0, arg0, arg1,
        number(spec.get('epoch', 0)), generation, 0, 0)
    return header + request + payload


def decode(packet: bytes) -> dict[str, Any]:
    if len(packet) < OUTPUT_BYTES:
        raise ValueError('truncated control response')
    words = OUTPUT.unpack_from(packet)
    version, operation, status, reply_len, sense_len, data_len, epoch, flags, sequence = words[:9]
    if version != VERSION or reply_len > 256 or sense_len > 96 or data_len > MAX_DATA or len(packet) != OUTPUT_BYTES + data_len:
        raise ValueError('invalid response lengths/version')
    data = packet[OUTPUT_BYTES:]
    result: dict[str, Any] = {'operation': operation, 'status': f'0x{status:08x}',
        'epoch': epoch, 'flags': flags, 'nextSequence': sequence,
        'replyHex': packet[64:64 + reply_len].hex(),
        'senseHex': packet[320:320 + sense_len].hex(), 'dataBytes': data_len}
    text = lambda b: b.split(b'\0', 1)[0].decode('utf-8', errors='replace')
    if operation == OPERATIONS['info'] and len(data) == 448:
        fields = struct.unpack_from('<8IQ6I', data)
        result['adapter'] = dict(zip(('pciId', 'capabilities', 'firmware', 'requestFrameBytes',
            'replyFrameBytes', 'requestCredits', 'targetLimit', 'featureBits', 'milliseconds',
            'mpiVersion', 'pciRevision'), fields[:11]))
        result['adapter']['pciId'] = f'0x{fields[0]:08x}'
        result['adapter']['iocFactsHex'] = data[64:192].hex()
        result['adapter']['pciConfigHex'] = data[192:448].hex()
    elif operation == OPERATIONS['targets']:
        if len(data) % 256:
            raise ValueError('misaligned target response')
        result['targets'] = []
        for offset in range(0, len(data), 256):
            values = struct.unpack_from('<HBBIQQQII', data, offset)
            target = dict(zip(('handle', 'kind', 'flags', 'generation', 'wwid', 'lun',
                'blocks', 'blockSize', 'maxTransfer'), values))
            target['wwid'] = f'0x{target["wwid"]:016x}'
            pos = offset + 40
            for key, length in (('vendor', 16), ('product', 24), ('revision', 8), ('serial', 32), ('identity', 128)):
                target[key] = text(data[pos:pos + length]); pos += length
            path_count, caps = struct.unpack_from('<II', data, offset + 248)
            target['pathCount'] = path_count
            target['managementOnly'] = bool(caps & 1)
            target['tlrEnabled'] = bool(caps & 2)
            target['protectionType'] = (caps >> 8) & 255
            result['targets'].append(target)
    elif operation == OPERATIONS['events']:
        if len(data) % 160:
            raise ValueError('misaligned event response')
        result['events'] = []
        for offset in range(0, len(data), 160):
            seq, milliseconds, event_epoch, event, context, length = struct.unpack_from('<QQ4I', data, offset)
            if length > 128:
                raise ValueError('event payload exceeds ABI')
            result['events'].append({'sequence': seq, 'milliseconds': milliseconds,
                'epoch': event_epoch, 'event': event, 'context': context,
                'payloadHex': data[offset + 32:offset + 32 + length].hex()})
    elif operation == OPERATIONS['diag-query'] and len(data) == 32:
        result['diagnostic'] = dict(zip(('type', 'uniqueId', 'bytes', 'flags', 'epoch'), struct.unpack_from('<5I', data)))
    elif data_len <= 4096:
        result['dataHex'] = data.hex()
    return result


class Connection:
    def __init__(self, registry_id: int | None, *, enumerate_only: bool = False) -> None:
        if sys.platform != 'darwin':
            raise RuntimeError('hardware access is macOS-only; --encode-only works on any host')
        self.io = C.CDLL('/System/Library/Frameworks/IOKit.framework/IOKit')
        self.system = C.CDLL('/usr/lib/libSystem.B.dylib')
        self.handle = 0
        signatures = {
            'IOServiceMatching': ([C.c_char_p], C.c_void_p),
            'IOServiceGetMatchingServices': ([C.c_uint32, C.c_void_p, C.POINTER(C.c_uint32)], C.c_uint32),
            'IOIteratorNext': ([C.c_uint32], C.c_uint32),
            'IOObjectRelease': ([C.c_uint32], C.c_uint32),
            'IORegistryEntryGetRegistryEntryID': ([C.c_uint32, C.POINTER(C.c_uint64)], C.c_uint32),
            'IOServiceOpen': ([C.c_uint32, C.c_uint32, C.c_uint32, C.POINTER(C.c_uint32)], C.c_uint32),
            'IOServiceClose': ([C.c_uint32], C.c_uint32),
            'IOConnectCallStructMethod': ([C.c_uint32, C.c_uint32, C.c_void_p, C.c_size_t,
                                          C.c_void_p, C.POINTER(C.c_size_t)], C.c_uint32)}
        for name, (args, result) in signatures.items():
            fn = getattr(self.io, name); fn.argtypes = args; fn.restype = result
        iterator = C.c_uint32()
        match = self.io.IOServiceMatching(b'MPSController')
        if not match:
            raise RuntimeError('cannot allocate service matching dictionary')
        self.check(self.io.IOServiceGetMatchingServices(0, match, C.byref(iterator)))
        entries: list[tuple[int, int]] = []
        try:
            for _ in range(256):
                service = self.io.IOIteratorNext(iterator.value)
                if not service:
                    break
                rid = C.c_uint64()
                status = self.io.IORegistryEntryGetRegistryEntryID(service, C.byref(rid))
                if status:
                    self.io.IOObjectRelease(service); self.check(status)
                entries.append((rid.value, service))
            self.adapters = [rid for rid, _ in entries]
            if enumerate_only:
                return
            chosen = [item for item in entries if registry_id is None or item[0] == registry_id]
            if len(chosen) != 1:
                raise RuntimeError('select exactly one adapter using --registry-id; use list to inspect IDs')
            connection = C.c_uint32()
            task = C.c_uint32.in_dll(self.system, 'mach_task_self_').value
            self.check(self.io.IOServiceOpen(chosen[0][1], task, CONNECT_TYPE, C.byref(connection)))
            self.handle = connection.value
        finally:
            for _, service in entries:
                self.io.IOObjectRelease(service)
            self.io.IOObjectRelease(iterator.value)

    @staticmethod
    def check(status: int) -> None:
        if status:
            raise RuntimeError(f'IOKit returned 0x{status:08x}; management requires administrator privilege and mps_mgmt=1')

    def close(self) -> None:
        if self.handle:
            self.io.IOServiceClose(self.handle); self.handle = 0

    def call(self, packet: bytes) -> bytes:
        if not self.handle:
            raise RuntimeError('connection is not open')
        capacity = OUTPUT_BYTES + INPUT.unpack_from(packet)[6]
        source = C.create_string_buffer(packet, len(packet))
        target = C.create_string_buffer(capacity)
        length = C.c_size_t(capacity)
        self.check(self.io.IOConnectCallStructMethod(self.handle, 0, source, len(packet), target, C.byref(length)))
        if length.value > capacity:
            raise RuntimeError('driver returned an invalid output length')
        result = target.raw[:length.value]
        decode(result)
        return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('list', 'info', 'targets', 'events', 'request'))
    parser.add_argument('spec', nargs='?', type=Path, help='JSON specification for request')
    parser.add_argument('--registry-id', type=lambda x: number(x, 64))
    parser.add_argument('--allow-mutation', action='store_true', help='explicitly acknowledge this command may change device state')
    parser.add_argument('--encode-only', type=Path, help='write packet only; never load IOKit or access hardware')
    parser.add_argument('--after', type=lambda x: number(x, 64), default=0)
    parser.add_argument('--output', type=Path, help='save response JSON')
    parser.add_argument('--data-output', type=Path, help='save the exact response payload')
    args = parser.parse_args()
    try:
        if args.operation == 'request':
            if not args.spec:
                parser.error('request requires a JSON specification')
            spec = json.loads(args.spec.read_text(encoding='utf-8'))
            if not isinstance(spec, dict):
                raise ValueError('request JSON must be an object')
            base = args.spec.parent
        else:
            spec = {'operation': args.operation, 'argument0': args.after}
            base = Path('.')
        if args.encode_only:
            if args.operation == 'list':
                raise ValueError('list has no control packet')
            packet = encode(spec, base=base, allow_mutation=args.allow_mutation)
            args.encode_only.write_bytes(packet)
            print(json.dumps({'packetBytes': len(packet), 'hardwareAccess': False}))
            return 0
        if args.allow_mutation and args.registry_id is None:
            raise ValueError('state-changing requests require an explicit --registry-id')
        connection = Connection(args.registry_id, enumerate_only=args.operation == 'list')
        try:
            if args.operation == 'list':
                print(json.dumps({'registryIds': [f'0x{x:x}' for x in connection.adapters]}, indent=2)); return 0
            if 'epoch' not in spec:
                info = decode(connection.call(encode({'operation': 'info'})))
                if info['status'] != '0x00000000':
                    raise RuntimeError(f'cannot obtain current transport epoch: {info["status"]}')
                spec['epoch'] = info['epoch']
            response = connection.call(encode(spec, base=base, allow_mutation=args.allow_mutation))
            result = decode(response)
            if args.data_output:
                args.data_output.write_bytes(response[OUTPUT_BYTES:])
            output = json.dumps(result, ensure_ascii=False, indent=2) + '\n'
            if args.output:
                args.output.write_text(output, encoding='utf-8', newline='')
            else:
                print(output, end='')
            return 0 if result['status'] == '0x00000000' else 1
        finally:
            connection.close()
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, struct.error) as exc:
        print(f'mpsctl: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
