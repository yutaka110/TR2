"""Build G4-02 with native action regressions and the deployable prediction probe."""
import argparse,json,os,subprocess,sys
from pathlib import Path
def main():
    p=argparse.ArgumentParser();p.add_argument('--name',required=True);p.add_argument('--build-name',default='reach_g4_prediction_20260924');a=p.parse_args()
    repo=Path(__file__).resolve().parents[1]
    subprocess.run([sys.executable,'tools/build_reach_actions.py','--name',a.name,'--build-name',a.build_name],cwd=repo,check=True)
    out=repo/'artifacts'/a.build_name/'verification'/a.name
    report=json.loads((out/'report.json').read_text());ms=report['commands'][0]['command'][0]
    command=[ms,str(repo/'tools/reach_prediction_probe.vcxproj'),'/nologo','/nr:false','/p:Configuration=Release','/p:Platform=x64','/v:minimal','/p:OutDir='+str(out/'prediction_bin')+os.sep,'/p:IntDir='+str(out/'prediction_obj')+os.sep]
    with (out/'prediction_build.txt').open('wb') as log:subprocess.run(command,cwd=repo,env=dict(os.environ),stdout=log,stderr=subprocess.STDOUT,check=True)
    r=subprocess.run([str(out/'prediction_bin/reach_prediction_probe.exe')],capture_output=True,check=True)
    (out/'prediction_tests.json').write_bytes(r.stdout);print(r.stdout.decode());return 0
if __name__=='__main__':sys.exit(main())
