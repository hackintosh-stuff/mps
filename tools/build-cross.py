#!/usr/bin/env python3
"""Compile genuine x86_64 Mach-O objects on Windows; never deploy/load.

This is separate from the macOS native link/sign entry point. LLVM's Mach-O
linker may not implement -kext; report that result rather than relabel a bundle.
"""
import argparse
import pathlib
import subprocess
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_LLVM = pathlib.Path(r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--llvm',type=pathlib.Path,default=DEFAULT_LLVM)
    args=parser.parse_args()
    out=ROOT/'_scratch/cross-build'
    out.mkdir(parents=True,exist_ok=True)
    logs=[]
    flags=['--target=x86_64-apple-macos10.15','-mkernel','-fapple-kext',
           '-mno-red-zone','-nostdinc++','-O2','-DKERNEL','-DKERNEL_PRIVATE',
           '-DDRIVER_PRIVATE','-D__APPLE_KEXT__','-fno-builtin','-fno-common',
           '-fno-exceptions','-fno-rtti','-fstack-protector','-fno-threadsafe-statics',
           '-std=gnu++17','-Werror=format','-Wno-unused-parameter','-Wno-unused-function',
           '-I',str(ROOT/'MacKernelSDK/Headers'),'-I',str(ROOT/'MPS')]
    failed=False
    objects=[]
    for source in sorted((ROOT/'MPS').glob('*.cpp')):
        obj=out/(source.stem+'.o')
        cmd=[str(args.llvm/'clang++.exe'),*flags,'-c',str(source),'-o',str(obj)]
        result=subprocess.run(cmd,capture_output=True,text=True,encoding='utf-8',errors='replace')
        logs.append('COMMAND: '+subprocess.list2cmdline(cmd)+'\n'+result.stdout+result.stderr+'\nexit='+str(result.returncode))
        print(source.name+': '+('FAIL' if result.returncode else 'OK'))
        failed |= result.returncode!=0
        if not result.returncode:
            magic,cpu,_,kind=struct.unpack_from('<4I',obj.read_bytes())
            assert (magic,cpu,kind)==(0xfeedfacf,0x1000007,1)
            objects.append(obj)
    # Resolve only our own symbols across objects. Kernel KPI imports remain.
    definitions=set(); undefined=set()
    for obj in objects:
        data=obj.read_bytes(); position=32
        for _ in range(struct.unpack_from('<I',data,16)[0]):
            command,size=struct.unpack_from('<II',data,position)
            if command==2:
                offset,count,strings,_=struct.unpack_from('<4I',data,position+8)
                for i in range(count):
                    name,typ,section,desc,value=struct.unpack_from('<IBBHQ',data,offset+i*16)
                    if not name or typ&0xe0: continue
                    end=data.index(0,strings+name)
                    symbol=data[strings+name:end].decode()
                    (definitions if typ&0x0e or value else undefined).add(symbol)
            position+=size
    missing=sorted(s for s in undefined-definitions if 'MPS' in s or s.startswith('_mps'))
    if missing:
        failed=True;logs.append('Undefined driver symbols: '+repr(missing))
    else:logs.append('Object symbol closure: PASS (not native link/load evidence)')
    logs.append('RESULT: '+('FAIL' if failed else 'PASS: Mach-O objects only; native link/sign/load NOT VERIFIED'))
    (out/'build.log').write_text('\n'.join(logs)+'\n',encoding='utf-8',newline='')
    print(logs[-1]); print(out/'build.log')
    return int(failed)

if __name__=='__main__': sys.exit(main())
