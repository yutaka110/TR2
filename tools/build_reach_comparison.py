"""Build G4-04 and all scheduler/prediction/action native regressions."""
import argparse,json,os,subprocess,sys
from pathlib import Path
def main():
    p=argparse.ArgumentParser();p.add_argument('--name',required=True);p.add_argument('--build-name',default='reach_g4_comparison_20260925');a=p.parse_args()
    repo=Path(__file__).resolve().parents[1]
    subprocess.run([sys.executable,'tools/build_reach_scheduler.py','--name',a.name,'--build-name',a.build_name],cwd=repo,check=True)
    out=repo/'artifacts'/a.build_name/'verification'/a.name;report=json.loads((out/'report.json').read_text());ms=report['commands'][0]['command'][0]
    cmd=[ms,str(repo/'tools/reach_comparison_probe.vcxproj'),'/nologo','/nr:false','/p:Configuration=Release','/p:Platform=x64','/v:minimal','/p:OutDir='+str(out/'comparison_bin')+os.sep,'/p:IntDir='+str(out/'comparison_obj')+os.sep]
    with (out/'comparison_build.txt').open('wb') as f:subprocess.run(cmd,cwd=repo,env=dict(os.environ),stdout=f,stderr=subprocess.STDOUT,check=True)
    r=subprocess.run([str(out/'comparison_bin/reach_comparison_probe.exe')],capture_output=True);(out/'comparison_tests.json').write_bytes(r.stdout);(out/'comparison_tests.stderr').write_bytes(r.stderr);print(r.stdout.decode()+r.stderr.decode());return r.returncode
if __name__=='__main__':sys.exit(main())
