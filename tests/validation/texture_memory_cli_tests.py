"""Check the actual texture-budget CLI parser without constructing the renderer."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import subprocess

STOP='--texture-memory-cli-test-stop'

def check(exe):
    cases=[]
    def invoke(arguments, expected=None, stop=True):
        command=[str(exe),*arguments,*([STOP] if stop else [])]
        result=subprocess.run(command,cwd=exe.parent,capture_output=True,text=True,errors='replace',timeout=20,
            creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        output=result.stdout+result.stderr
        assert result.returncode!=0,(arguments,output)
        if expected is None:
            assert 'unknown argument: '+STOP in output,(arguments,output)
        else:
            assert expected in output and STOP not in output,(arguments,output)
        cases.append(dict(arguments=arguments,valid=expected is None,returnCode=result.returncode,diagnostic=output.strip()))
    for value in ('1','512','1024','2048','16384'):
        invoke(['--texture-memory-mb',value])
    invoke(['--texture-memory-mb','1','--texture-memory-mb','16384'])
    for value in ('','0','-1','+1','1.0','1junk',' 1','1 ','16385','4294967296','18446744073709551615','nan','inf'):
        invoke(['--texture-memory-mb',value],'--texture-memory-mb must be an integer in [1,16384] MiB')
    invoke(['--texture-memory-mb'],'missing value for --texture-memory-mb',stop=False)
    help_result=subprocess.run([str(exe),'--help'],cwd=exe.parent,capture_output=True,text=True,errors='replace',timeout=20,
        creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    assert help_result.returncode==0 and '--texture-memory-mb' in help_result.stdout and '[1,16384] MiB (1024)' in help_result.stdout
    cases.append(dict(arguments=['--help'],valid=True,returnCode=0,defaultBudgetMiB=1024))
    return dict(passed=True,gpuInitialized=False,caseCount=len(cases),cases=cases)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe',required=True,type=Path);parser.add_argument('--output',type=Path)
    args=parser.parse_args();result=check(args.exe.resolve(strict=True))
    if args.output:
        args.output.parent.mkdir(parents=True,exist_ok=True)
        with args.output.open('x',encoding='utf-8') as stream:json.dump(result,stream,indent=2);stream.write('\n')
    print('PASS',result['caseCount'],'texture-memory CLI cases; GPU not initialized')

if __name__=='__main__':main()
