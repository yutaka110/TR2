"""Isolated G4-01 build and native tests; no launch or original evidence mutation."""
import argparse,hashlib,json,os,subprocess,sys
from pathlib import Path
from datetime import datetime

def main():
    p=argparse.ArgumentParser();p.add_argument('--build-name',default='reach_g4_actions_20260924');p.add_argument('--name',default='build_01');a=p.parse_args()
    for x in (a.name,a.build_name):
        if not x.replace('_','').isalnum():raise ValueError('unsafe name')
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/a.build_name;out=root/'verification'/a.name;out.mkdir(parents=True,exist_ok=False)
    vs=Path(os.environ['ProgramFiles(x86)'])/'Microsoft Visual Studio/Installer/vswhere.exe'
    ms=subprocess.check_output([str(vs),'-latest','-products','*','-requires','Microsoft.Component.MSBuild','-find','MSBuild/**/Bin/MSBuild.exe'],text=True).splitlines()[0]
    report=dict(passed=False,started_at=datetime.now().astimezone().isoformat(),commands=[])
    commands=[[ms,str(repo/'tools/reach_actions_tests.vcxproj'),'/nologo','/nr:false','/p:Configuration=Release','/p:Platform=x64','/v:minimal','/p:OutDir='+str(out/'bin')+os.sep,'/p:IntDir='+str(out/'obj')+os.sep],
              [str(out/'bin/reach_actions_tests.exe')],
              [ms,str(repo/'TR2.sln'),'/nologo','/m:1','/nr:false','/t:Build','/p:Configuration=Release','/p:Platform=x64','/p:ReachG0Root='+str(root),'/p:ForceImportBeforeCppTargets='+str(repo/'tools/reach_g0_paths.props'),'/verbosity:minimal']]
    for i,cmd in enumerate(commands):
        print(f'Build/check {i+1}/3',flush=True)
        with (out/f'command_{i}.txt').open('wb') as log:r=subprocess.run(cmd,cwd=repo,env=dict(os.environ),stdout=log,stderr=subprocess.STDOUT)
        report['commands'].append(dict(command=cmd,exit_code=r.returncode))
        if r.returncode:break
    else:
        report['passed']=True;report['executable_sha256']=hashlib.sha256((root/'bin/Release/GE3.exe').read_bytes()).hexdigest()
        report['source_hashes']={f.as_posix():hashlib.sha256((repo/f).read_bytes()).hexdigest() for folder in ('network','research') for f in (Path(folder)).glob('*') if f.suffix in ('.cpp','.h')}
    (out/'report.json').write_text(json.dumps(report,indent=2),encoding='utf-8');print(json.dumps(dict(passed=report['passed'])))
    return 0 if report['passed'] else 1
if __name__=='__main__':sys.exit(main())
