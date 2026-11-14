#!/usr/bin/env python3
"""Offline SAS2 management CLI wire checks; never loads IOKit."""
from pathlib import Path
import importlib.util
import struct

path = Path(__file__).with_name('mpsctl.py')
spec = importlib.util.spec_from_file_location('mpsctl_offline', path)
cli = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cli)

def request(spec):
    packet = cli.encode(spec)
    header = cli.INPUT.unpack_from(packet)
    assert len(packet) == 64 + header[4] + header[5]
    return packet[64:64 + header[4]]

assert cli.CONNECT_TYPE == 0x4D505301
fw = request({'function': 'fw-download', 'imageType': 1, 'dataOutHex': 'deadbeef',
              'downloadToken': 7, 'imageOffset': 4, 'totalImageBytes': 8})
assert fw == bytes.fromhex(
    '0100000900000001000000000800000000000000'
    '00000c00000000000400000004000000')
upload = request({'function': 'fw-upload', 'imageType': 2, 'imageOffset': 512, 'readBytes': 4096})
assert len(upload) == 36 and upload[:4] == bytes((2, 0, 0, 0x12))
assert upload[20:28] == bytes.fromhex('00000c0000000000')
assert struct.unpack_from('<II', upload, 28) == (512, 4096)
query = request({'function': 'fw-upload', 'imageType': 1})
assert query[20:] == bytes.fromhex('00000c00000000000000000000000000')
smp = request({'function': 'smp', 'physicalPort': 255, 'sasAddress': '0x500a0d10012d17e0',
               'dataOutHex': '40000000', 'readBytes': 32})
assert len(smp) == 32 and smp[3] == 0x1a and smp[6] == 0
assert smp[16:24] == bytes.fromhex('e0172d01100d0a50')
fis = '2780ec' + '00' * 17
sata = request({'function': 'sata', 'handle': 9, 'fisHex': fis, 'readBytes': 512})
assert len(sata) == 48 and sata[3] == 0x1c and sata[6] == 0
assert sata[28:48] == bytes.fromhex(fis)
tool = request({'function': 'toolbox-cli', 'commandHex': '11223344', 'readBytes': 32})
assert len(tool) == 112 and tool[3] == 0x17 and tool[0] == 6
assert tool[12] == 0 and tool[20:24] == bytes.fromhex('11223344')
scsi = request({'function': 'scsi', 'handle': 9, 'lun': 0x123456, 'cdbHex': '120000002400', 'readBytes': 36})
assert scsi[52:60] == bytes.fromhex('d212345600000000')
assert scsi[36:38] == bytes((6, 0))
for bad in (
    {'function': 'nvme', 'commandHex': '00' * 64},
    {'requestHex': '000000330000000000000000'},
    {'requestHex': '000000c80000000000000000'},
    {'function': 'config', 'pageType': 15, 'extendedPageType': 0x1d},
    {'function': 'fw-upload', 'readBytes': 0x1000000},
    {'function': 'fw-download', 'dataOutHex': '00', 'downloadToken': 0},
    {'function': 'fw-download', 'dataOutHex': '00', 'downloadToken': 1, 'imageOffset': 1, 'totalImageBytes': 1},
):
    try:
        cli.encode(bad)
    except (ValueError, KeyError):
        pass
    else:
        raise AssertionError('accepted invalid request: ' + repr(bad))
assert len(request({'function': 'fw-upload', 'readBytes': 0xffffff})) == 36
for p in (path, Path(__file__), Path(__file__).with_suffix('.cpp'), path.parent.parent/'MPS/MPSControl.cpp', path.parent.parent/'MPS/MPI/mpi2_mgmt.h'):
    assert b'\r' not in p.read_bytes(), str(p)
print('management CLI: firmware TC golden bytes, SAS2 prefixes, LUN, independent identifier, unsupported opcode/24-bit boundaries PASS (offline only)')
