"""Serial regressions for the opt-in G4-02 predictor; no concurrent live load."""
import argparse,json,subprocess,sys
from pathlib import Path
from verify_reach_closed_loop import save
from verify_reach_command import require
from verify_reach_baseline import baseline_audit
from verify_reach_state import state_audit
from verify_reach_budget import budget_audit
from verify_reach_decode import decode_audit
from verify_reach_integration import integration_audit
def main():
    p=argparse.ArgumentParser();p.add_argument('--build-name',default='reach_g4_prediction_20260924');a=p.parse_args();require(a.build_name.replace('_','').isalnum(),'unsafe name')
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/a.build_name;out=root/'verification/regression_01';out.mkdir(parents=True,exist_ok=False)
    report=dict(passed=False,commands=[])
    commands=[['tools/verify_reach_visual_units.py','--name','regression_units_01','--include-command','--include-link','--include-state','--include-baseline'],
              ['tools/verify_reach_actions.py','--name','actions_regression_01'],
              ['tools/verify_reach_g1_legacy.py','--name','legacy_regression_01'],
              ['tools/run_reach_g1.py','--name','b3_regression_01','--stage','command_udp','--task','T2','--duration','8','--baseline','B3','--baseline-lambda','1','--state-feedback','--packet-trace','--process-priority','above_normal','--precise-wait','--budget-config',str(repo/'config/reach_rt_g2_budget.json'),'--link-config',str(root/'verification/actions_regression_01/normal_link.json')]]
    try:
        for i,command in enumerate(commands):
            cmd=[sys.executable]+command+['--build-name',a.build_name]
            with (out/f'command_{i}.txt').open('wb') as f:r=subprocess.run(cmd,cwd=repo,stdout=f,stderr=subprocess.STDOUT)
            report['commands'].append(dict(command=cmd,exit_code=r.returncode));require(r.returncode==0,'regression failed '+command[0]);print(command[0]+' passed',flush=True)
        session=next((root/'runs/b3_regression_01/sessions').iterdir())
        report['B3']=dict(session=str(session),baseline=baseline_audit(session),state=state_audit(session),budget=budget_audit(session),decode=decode_audit(session),integration=integration_audit(session,out/'b3'))
        require(not report['B3']['integration']['allowance_violations'],'B3 pose allowance');report['passed']=True
    except Exception as e:report['error']=repr(e);print(repr(e),flush=True)
    save(out/'report.json',report);return 0 if report['passed'] else 1
if __name__=='__main__':sys.exit(main())
