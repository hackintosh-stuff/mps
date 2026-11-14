#!/usr/bin/env python3
"""Run real compilation and host protocol suites; no hardware is accessed."""
from pathlib import Path
from datetime import datetime, timezone
import os
import shlex
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'_scratch/final-validation'

def main():
    OUT.mkdir(parents=True,exist_ok=True)
    results=[]
    def run(label,cmd):
        start=datetime.now(timezone.utc).isoformat()
        p=subprocess.run(cmd,cwd=ROOT,capture_output=True,text=True,encoding='utf-8',errors='replace')
        log='START '+start+'\nCOMMAND '+repr(cmd)+'\n'+p.stdout+p.stderr+'\nEXIT '+str(p.returncode)+'\nEND '+datetime.now(timezone.utc).isoformat()+'\n'
        (OUT/(label+'.log')).write_text(log,encoding='utf-8',newline='')
        results.append((label,p.returncode))
        print(label+': '+str(p.returncode),flush=True)
        if p.stdout: print(p.stdout.strip(),flush=True)
        if p.returncode: print(p.stderr[-4000:],flush=True)
    run('build-inputs',[sys.executable,'tools/validate-build-inputs.py'])
    if os.name=='nt':run('cross-build',[sys.executable,'tools/build-cross.py'])
    else:run('native-build',['sh','tools/build-noxcode.sh'])
    run('management-cli',[sys.executable,'tools/test_management.py'])
    for suite in ('sas2','protocol','management'):
        cmd=('set -eu; g++ -std=c++17 -O1 -g -Wall -Wextra -Werror '
             '-fsanitize=address,undefined tools/test_'+suite+'.cpp -o /tmp/mps-final-'+suite+
             '; /tmp/mps-final-'+suite)
        run('host-'+suite,['wsl.exe','-d','Ubuntu','--','bash','-c',cmd] if os.name=='nt' else ['sh','-c',cmd])
    summary='\n'.join(label+' exit='+str(rc) for label,rc in results)+'\n'
    (OUT/'summary.txt').write_text(summary,encoding='utf-8',newline='')
    print(summary)
    return int(any(rc for _,rc in results))

if __name__=='__main__':sys.exit(main())
