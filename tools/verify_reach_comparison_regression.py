"""Old scheduler, predictor, baseline and legacy regressions, run after live comparisons."""
import argparse,subprocess,sys
from pathlib import Path
from verify_reach_closed_loop import save
from verify_reach_command import require
from verify_reach_scheduler import scheduler_audit
from verify_reach_actions import action_audit
from verify_reach_budget import budget_audit
from verify_reach_decode import decode_audit
from verify_reach_state import state_audit
from verify_reach_integration import integration_audit
def main():
    p=argparse.ArgumentParser();p.add_argument('--build-name',default='reach_g4_comparison_20260925');a=p.parse_args();repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/a.build_name;out=root/'verification/comparison_regression_01';out.mkdir(parents=True,exist_ok=False)
    report=dict(passed=False,commands=[])
    try:
        commands=[['tools/verify_reach_scheduler_regression.py','--build-name',a.build_name],
            ['tools/run_reach_g1.py','--name','g403_regression_01','--build-name',a.build_name,'--stage','command_udp','--task','T2','--duration','8','--baseline','G4-03','--state-feedback','--packet-trace','--process-priority','above_normal','--precise-wait','--budget-config','config/reach_rt_g2_budget.json','--link-config',str(root/'verification/actions_regression_01/normal_link.json'),'--prediction-model','artifacts/reach_g4_prediction_20260924/verification/study_02/paths.csv']]
        for i,command in enumerate(commands):
            with (out/f'command_{i}.txt').open('wb') as f:r=subprocess.run([sys.executable]+command,cwd=repo,stdout=f,stderr=subprocess.STDOUT)
            report['commands'].append(dict(command=command,exit_code=r.returncode));require(r.returncode==0,'regression failed '+command[0]);print(command[0]+' passed',flush=True)
        s=next((root/'runs/g403_regression_01/sessions').iterdir());report['G4-03']=dict(session=str(s),scheduler=scheduler_audit(s),action=action_audit(s),budget=budget_audit(s),decode=decode_audit(s),state=state_audit(s),integration=integration_audit(s,out/'g403'))
        require(not report['G4-03']['integration']['allowance_violations'],'G403 pose allowance');report['passed']=True
    except Exception as e:report['error']=repr(e);print(repr(e),flush=True)
    save(out/'report.json',report);return 0 if report['passed'] else 1
if __name__=='__main__':sys.exit(main())
