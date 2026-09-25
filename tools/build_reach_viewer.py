"""Build the navigation UI into a separate directory from frozen research evidence."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path

repo=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--configuration',choices=('Debug','Development','Release'),default='Release')
parser.add_argument('--build-name',default='reach_viewer_20260925')
args=parser.parse_args()
if not args.build_name.replace('_','').isalnum():parser.error('invalid build name')
root=repo/'artifacts'/args.build_name;out=root/'verification';out.mkdir(parents=True,exist_ok=True)
vs=Path(os.environ['ProgramFiles(x86)'])/'Microsoft Visual Studio/Installer/vswhere.exe'
ms=subprocess.check_output([str(vs),'-latest','-products','*','-requires','Microsoft.Component.MSBuild','-find','MSBuild/**/Bin/MSBuild.exe'],text=True).splitlines()[0]
cmd=[ms,str(repo/'TR2.sln'),'/nologo','/m:1','/nr:false','/t:Build','/p:Configuration='+args.configuration,'/p:Platform=x64','/p:ReachG0Root='+str(root),'/p:ForceImportBeforeCppTargets='+str(repo/'tools/reach_g0_paths.props'),'/verbosity:minimal']
# os.environ normalizes Windows environment keys; avoid duplicate Path/PATH from a host shell.
with (out/'build_python.log').open('wb') as f:r=subprocess.run(cmd,cwd=repo,env=dict(os.environ),stdout=f,stderr=subprocess.STDOUT)
report=dict(passed=r.returncode==0,configuration=args.configuration,command=cmd,exit_code=r.returncode)
if r.returncode==0:
 report['executable_sha256']=hashlib.sha256((root/'bin'/args.configuration/'GE3.exe').read_bytes()).hexdigest()
 report['source_hashes']={f:hashlib.sha256((repo/f).read_bytes()).hexdigest() for f in ['GE3.vcxproj','application/AppMain.cpp','application/AppMain.h','application/AppImGuiLayer.cpp','application/ReachReportNavigation.h','application/ReachLiveNavigation.cpp','application/ReachLiveNavigation.h','engine/src/platform/Window.cpp','research/ReachFoundation.cpp','research/ReachFoundation.h','research/ReachResearchMode.cpp','config/reach_rt_live_demo.json','docs/Reach_RT_Verification.html']}
(out/'build_report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps(dict(passed=report['passed'],log=str(out/'build_python.log'))))
raise SystemExit(r.returncode)
