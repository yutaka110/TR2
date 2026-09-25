"""Run a new trial using a qualified G3-02 mode's fixed coefficient and conditions."""
import argparse,subprocess,sys
from datetime import datetime
from pathlib import Path
from run_reach_baseline_study import sha
from verify_reach_closed_loop import read

def main():
    p=argparse.ArgumentParser();p.add_argument('--mode',choices=['B1','B2','B3'],required=True);p.add_argument('--case',choices=['valid_T1_normal','valid_T2_normal','valid_T1_mixed','valid_T2_small_queue'],default='valid_T2_normal');p.add_argument('--name');p.add_argument('--show',action='store_true');p.add_argument('--print-command',action='store_true');a=p.parse_args()
    repo=Path(__file__).resolve().parents[1];build='reach_g3_ablation_v5_20260923';root=repo/'artifacts'/build;out=root/'verification/study_01'
    if not read(out/'task_decision.json')['passed']:raise RuntimeError('G3-02 not qualified')
    lock=read(out/'baseline_lock.json')
    if sha(root/'bin/Release/GE3.exe')!=lock['executable_sha256']:raise RuntimeError('fixed executable changed')
    c=next(c for c in read(out/'plan.json')['cases'] if c['name']==a.case);weight=read(out/'selection.json')[a.mode]['lambda']
    name=a.name or 'demo_'+a.mode+'_'+datetime.now().strftime('%Y%m%d_%H%M%S')
    cmd=[sys.executable,str(repo/'tools/run_reach_g1.py'),'--stage','command_udp','--task',c['task'],'--duration','60','--name',name,'--build-name',build,'--baseline',a.mode,'--baseline-lambda',str(weight),'--state-feedback','--packet-trace','--budget-config',str(out/'budget.json'),'--process-priority','above_normal','--timeout','95','--precise-wait']
    cmd+=['--trace-bundle',str(out/(c['name']+'_trace'))] if 'seed' in c else ['--link-config',str(out/(c['name']+'_link.json'))]
    if a.show:cmd.append('--show')
    if a.print_command:print(subprocess.list2cmdline(cmd));return 0
    return subprocess.call(cmd,cwd=repo)

if __name__=='__main__':raise SystemExit(main())
