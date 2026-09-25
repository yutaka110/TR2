"""One postregistered paired repeat after a camera-clock invalidation.

Retain the original 20-trial report unchanged. No retries inside this supplement.
Both B0 and B1 are measured again; never splice a successful half-pair silently.
"""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from verify_reach_closed_loop import read, save
from verify_reach_command import require
from verify_reach_integration import integration_audit
from verify_reach_budget import budget_audit
from verify_reach_decode import decode_audit
from verify_reach_state import state_audit
from verify_reach_baseline import baseline_audit
from audit_reach_stage_conservation import stage_conservation


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p=argparse.ArgumentParser();p.add_argument('--name',default='supplement_01');p.add_argument('--build-name',default='reach_g3_baseline_v3_20260922');args=p.parse_args()
    for x in (args.name,args.build_name):require(x.replace('_','').replace('-','').isalnum(),'unsafe name')
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/args.build_name;study=root/'verification/study_01';out=root/'verification'/args.name
    out.mkdir(parents=True,exist_ok=False)
    original=read(study/'report.json');selection=read(study/'selection.json');lock=read(study/'baseline_lock.json')
    invalid=[r for r in original['trials'] if not r['passed']]
    require(original['complete'] and len(invalid)==1 and invalid[0]['id']=='valid_T2_normal_B0_0p1','unexpected original failure set')
    failed=read(Path(invalid[0]['session'])/'summary.json')
    require(failed['status']=='invalid' and failed['reason']=='camera deadline missed; historical pixels cannot be reconstructed','not a camera-clock invalidation')
    require(sha(root/'bin/Release/GE3.exe')==lock['executable_sha256'],'fixed executable changed')
    case=next(c for c in read(study/'plan.json')['cases'] if c['name']=='valid_T2_normal')
    plan=dict(created_at=datetime.now().astimezone().isoformat(),reason=failed,original_report_sha256=sha(study/'report.json'),
        selection_sha256=sha(study/'selection.json'),executable_sha256=lock['executable_sha256'],case=case,order=['B0','B1'],duration_s=60,
        retries=0,coefficient_changes=False,scope='additional paired full-window confirmation; original invalid and unpaired B1 remain visible; not new tuning or statistical efficacy')
    save(out/'plan.json',plan);report=dict(passed=False,complete=False,trials=[])
    for mode in plan['order']:
        name=args.name+'_'+mode;weight=.1 if mode=='B0' else selection['lambda']
        item=dict(id=name,case=case['name'],mode=mode,**{'lambda':weight},phase='validation',supplemental=True,passed=False,started_at=datetime.now().astimezone().isoformat())
        report['trials'].append(item);save(out/'report.json',report);print(json.dumps(dict(starting=name)),flush=True)
        try:
            command=[sys.executable,str(repo/'tools/run_reach_g1.py'),'--stage','command_udp','--task','T2','--duration','60','--name',name,'--build-name',args.build_name,
                '--link-config',str(study/'valid_T2_normal_link.json'),'--budget-config',str(study/'budget.json'),'--state-feedback','--packet-trace',
                '--baseline',mode,'--baseline-lambda',str(weight),'--timeout','95']
            result=subprocess.run(command,cwd=repo,capture_output=True);(out/(name+'.stdout.txt')).write_bytes(result.stdout);(out/(name+'.stderr.txt')).write_bytes(result.stderr)
            invocation=root/'runs'/name;session=next((invocation/'sessions').iterdir());item['session']=str(session)
            require(result.returncode==0,'native supplemental run failed')
            require(read(invocation/'launch.json')['executable_sha256']==lock['executable_sha256'],'launch executable changed')
            hashes=read(invocation/'source_hashes.json');require(all(hashes[k]==v for k,v in lock['native_source_sha256'].items()),'native sources changed')
            effective=read(session/'config.effective.json');require(effective==read(invocation/'requested_config.json') and effective['link_model']==case['link'] and effective['ip_budget']==lock['budget'],'supplemental conditions changed')
            item['baseline']=baseline_audit(session);item['integration']=integration_audit(session,out/name);item['budget']=budget_audit(session)
            item['decode']=decode_audit(session);item['state']=state_audit(session);terminal=stage_conservation(session);save(out/name/'frame_terminal.json',terminal);item['frame_terminal_counts']=terminal['counts']
            require(not item['integration']['allowance_violations'] and item['integration']['motion']['outcome']=='success','normal supplemental task failed')
            item['passed']=True
        except Exception as e:item['error']=str(e)
        item['finished_at']=datetime.now().astimezone().isoformat();save(out/'report.json',report);print(json.dumps(dict(finished=name,passed=item['passed'],error=item.get('error'))),flush=True)
    report['complete']=True;report['passed']=all(r['passed'] for r in report['trials']);save(out/'report.json',report)
    return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
