#!/usr/bin/env python3
"""Attempt a genuine unsigned cross kext link; never relabel another Mach-O type.

This tool is an optional link-capability check after build-cross.py. It does
not deploy, sign, load, change EFI, or alter source/object file headers.
An unsupported -kext or a non-MH_KEXT_BUNDLE output is a hard failure.
"""
import argparse
from pathlib import Path
import plistlib
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_LLVM = Path(r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin')


def symbols(path):
    raw = path.read_bytes()
    magic, cpu, _, kind, commands = struct.unpack_from('<5I', raw)
    if (magic, cpu) != (0xfeedfacf, 0x1000007):
        raise ValueError('expected x86_64 Mach-O: ' + str(path))
    definitions, undefined = set(), set()
    position = 32
    for _ in range(commands):
        command, size = struct.unpack_from('<II', raw, position)
        if size < 8 or position + size > len(raw):
            raise ValueError('invalid Mach-O load command')
        if command == 2:
            offset, count, strings, _ = struct.unpack_from('<4I', raw, position + 8)
            for index in range(count):
                name, typ, section, desc, value = struct.unpack_from('<IBBHQ', raw, offset + 16 * index)
                if not name or typ & 0xe0:
                    continue
                end = raw.index(0, strings + name)
                text = raw[strings + name:end].decode()
                (definitions if typ & 0x0e or value else undefined).add(text)
        position += size
    return kind, definitions, undefined


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--llvm', type=Path, default=DEFAULT_LLVM)
    parser.add_argument('--objects', type=Path, default=ROOT / '_scratch/cross-build')
    args = parser.parse_args()
    work = ROOT / '_scratch/cross-link'
    work.mkdir(parents=True, exist_ok=True)
    logs = []

    def run(command):
        result = subprocess.run([str(part) for part in command], capture_output=True)
        logs.append('COMMAND: ' + subprocess.list2cmdline([str(part) for part in command]) + '\n' +
                    (result.stdout + result.stderr).decode('utf-8', errors='replace') + '\nexit=' + str(result.returncode))
        return result.returncode

    try:
        info = (ROOT / 'MPS/Info.plist').read_bytes()
        version = plistlib.loads(info)['CFBundleVersion']
        sources = sorted((ROOT / 'MPS').glob('*.cpp'))
        objects = [args.objects / (source.stem + '.o') for source in sources]
        for source, obj in zip(sources, objects):
            if not obj.is_file():
                raise ValueError('missing driver object: ' + str(obj))
            if obj.stat().st_mtime_ns < source.stat().st_mtime_ns:
                logs.append('WARNING: object predates current source; this link is a toolchain capability check only: ' + source.name)
            if symbols(obj)[0] != 1:
                raise ValueError('input is not MH_OBJECT: ' + str(obj))
        kmod_source = work / 'kmod-info.cpp'
        kmod_source.write_text('''#include <mach/kmod.h>
#include <libkern/OSKextLib.h>
extern "C" {
extern kern_return_t _start(kmod_info_t *, void *);
extern kern_return_t _stop(kmod_info_t *, void *);
KMOD_EXPLICIT_DECL(com.maz1.MPS, "''' + version + '''", _start, _stop);
__attribute__((visibility("hidden"))) kmod_start_func_t *_realmain = nullptr;
__attribute__((visibility("hidden"))) kmod_stop_func_t *_antimain = nullptr;
__attribute__((visibility("hidden"))) int _kext_apple_cc = __APPLE_CC__;
}
''', encoding='utf-8', newline='')
        kmod_obj = work / 'kmod-info.o'
        flags = ['--target=x86_64-apple-macos10.15', '-mkernel', '-fapple-kext', '-mno-red-zone',
                 '-nostdinc++', '-DKERNEL', '-DKERNEL_PRIVATE', '-DDRIVER_PRIVATE', '-D__APPLE_KEXT__',
                 '-fno-builtin', '-fno-common', '-fno-exceptions', '-fno-rtti', '-std=gnu++17',
                 '-I', ROOT / 'MacKernelSDK/Headers', '-c', kmod_source, '-o', kmod_obj]
        if run([args.llvm / 'clang++.exe', *flags]):
            raise ValueError('kmod metadata object compilation failed')
        candidate = work / 'MPS.link-attempt'
        # A stale candidate can never count as the output of a failed linker.
        if candidate.is_file():
            candidate.unlink()
        if run([args.llvm / 'ld64.lld.exe', '-kext', '-arch', 'x86_64',
                '-platform_version', 'macos', '10.15', '11.0', *objects, kmod_obj,
                ROOT / 'MacKernelSDK/Library/universal/libkmodc++.a',
                ROOT / 'MacKernelSDK/Library/x86_64/libkmod.a', '-o', candidate]):
            raise ValueError('genuine -kext link failed; no kext artifact was produced')
        kind, defined, undefined = symbols(candidate)
        if kind != 11:
            raise ValueError('linker produced filetype ' + str(kind) + ', expected MH_KEXT_BUNDLE=11; output rejected without header modification')
        required = {'_kmod_info', '__start', '__stop'}
        if not required <= defined:
            raise ValueError('missing kmod entry symbols: ' + repr(sorted(required - defined)))
        missing = sorted(name for name in undefined if 'MPS' in name or name.startswith('_mps'))
        if missing:
            raise ValueError('undefined driver symbols: ' + repr(missing))
        bundle = work / 'MPS.kext/Contents'
        (bundle / 'MacOS').mkdir(parents=True, exist_ok=True)
        (bundle / 'MacOS/MPS').write_bytes(candidate.read_bytes())
        (bundle / 'Info.plist').write_bytes(info)
        logs.append('RESULT: PASS genuine MH_KEXT_BUNDLE, kmod entries and driver-symbol closure; cross-linked UNSIGNED; native link/sign/load UNVERIFIED')
        return_code = 0
    except (OSError, ValueError, struct.error) as error:
        logs.append('RESULT: FAIL: ' + str(error) + '; native macOS link/sign/load remains UNVERIFIED')
        return_code = 1
    (work / 'link.log').write_text('\n\n'.join(logs) + '\n', encoding='utf-8', newline='')
    print(logs[-1])
    print(work / 'link.log')
    return return_code


if __name__ == '__main__':
    sys.exit(main())
