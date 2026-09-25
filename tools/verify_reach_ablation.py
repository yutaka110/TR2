"""Short real-UDP checks for masking, stale fallback and notification faults."""
import argparse,json,subprocess,sys
from pathlib import Path
from verify_reach_closed_loop import read,save,rows
from verify_reach_command import require
from verify_reach_baseline import baseline_audit
from verify_reach_state import state_audit
from verify_reach_budget import budget_audit
from verify_reach_decode import decode_audit
from verify_reach_integration import integration_audit,direction
from audit_reach_ablation_state import ablation_summary


def main():
    p=argparse.ArgumentParser();p.add_argument('--name',required=True);p.add_argument('--build-name',default='reach_g3_ablation_v5_20260923');p.add_argument('--remaining',action='store_true');a=p.parse_args()
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/a.build_name;out=root/'verification'/a.name;out.mkdir(parents=True,exist_ok=False)
    report=dict(passed=False,cases=[])
    cases=[('B1','normal'),('B2','normal'),('B3','normal'),('B2','blackout'),('B3','blackout'),('B2','expired'),('B3','no_notification'),('B2','invalid'),('B3','duplicate')]
    if a.remaining:cases=cases[5:]
    try:
        for mode,kind in cases:
            name=mode+'_'+kind;down=direction()
            if kind=='blackout':down['impairment']=[dict(at_us=0,delay_us=20000,drop=False),dict(at_us=1500000,delay_us=0,drop=True),dict(at_us=4500000,delay_us=20000,drop=False)]
            if kind=='expired':down['impairment']=[dict(at_us=0,delay_us=300000,drop=False)]
            if kind=='no_notification':down['impairment']=[dict(at_us=0,delay_us=0,drop=True)]
            save(out/(name+'.json'),dict(uplink=direction(),downlink=down))
            cmd=[sys.executable,str(repo/'tools/run_reach_g1.py'),'--name',a.name+'_'+name,'--build-name',a.build_name,'--stage','command_udp','--task','T2','--duration','8','--baseline',mode,'--baseline-lambda','.3','--state-feedback','--packet-trace','--process-priority','above_normal','--precise-wait','--budget-config',str(repo/'config/reach_rt_g2_budget.json'),'--link-config',str(out/(name+'.json'))]
            if kind in ('invalid','duplicate'):cmd+=['--state-diagnostic',kind]
            run=subprocess.run(cmd,cwd=repo,capture_output=True);(out/(name+'.stdout')).write_bytes(run.stdout);(out/(name+'.stderr')).write_bytes(run.stderr)
            session=next((root/'runs'/(a.name+'_'+name)/'sessions').iterdir());entry=dict(name=name,session=str(session),passed=False);report['cases'].append(entry)
            require(run.returncode==0,'native preflight failed')
            entry.update(baseline=baseline_audit(session),state=state_audit(session),budget=budget_audit(session),decode=decode_audit(session),integration=integration_audit(session,out/name),ablation=ablation_summary(session))
            ds=rows(session/'baseline_decisions.csv')
            if kind=='normal' and mode!='B1':require(entry['ablation']['live_state_decisions']>0 and entry['ablation']['value_changed']>0,'state has no effect on values')
            if kind in ('expired','no_notification'):require(all(d['state_live']=='0' and d['state_value']=='1' and d['selected']==d['b1_selected'] for d in ds),'fallback differs from B1')
            if kind=='blackout':require(entry['state']['stale_estimates']>20 and any(d['state_live']=='0' and int(d['state_id'])>10 for d in ds),'silence did not expire policy state')
            entry['passed']=True;save(out/'report.json',report);print(json.dumps(dict(case=name,passed=True,ablation=entry['ablation'])),flush=True)
        report['passed']=True
    except Exception as e:report['error']=str(e);print(str(e),flush=True)
    save(out/'report.json',report);return 0 if report['passed'] else 1

if __name__=='__main__':raise SystemExit(main())
