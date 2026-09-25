"""Build the actual scheduler executable plus native prediction/action/scheduler checks."""
import argparse,json,os,subprocess,sys
from pathlib import Path
def main():
    p=argparse.ArgumentParser();p.add_argument('--name',required=True);p.add_argument('--build-name',default='reach_g4_scheduler_20260925');p.add_argument('--units-only',action='store_true');a=p.parse_args()
    repo=Path(__file__).resolve().parents[1]
    if not a.units_only:subprocess.run([sys.executable,'tools/build_reach_prediction.py','--name',a.name,'--build-name',a.build_name],cwd=repo,check=True)
    out=repo/'artifacts'/a.build_name/'verification'/a.name
    report=json.loads((out/'report.json').read_text());ms=report['commands'][0]['command'][0]
    command=[ms,str(repo/'tools/reach_scheduler_tests.vcxproj'),'/nologo','/nr:false','/p:Configuration=Release','/p:Platform=x64','/v:minimal','/p:OutDir='+str(out/'scheduler_bin')+os.sep,'/p:IntDir='+str(out/'scheduler_obj')+os.sep]
    with (out/'scheduler_build.txt').open('wb') as log:subprocess.run(command,cwd=repo,env=dict(os.environ),stdout=log,stderr=subprocess.STDOUT,check=True)
    r=subprocess.run([str(out/'scheduler_bin/reach_scheduler_tests.exe')],capture_output=True)
    (out/'scheduler_tests.json').write_bytes(r.stdout);(out/'scheduler_tests.stderr').write_bytes(r.stderr)
    print(r.stdout.decode()+r.stderr.decode());return r.returncode
if __name__=='__main__':sys.exit(main())
