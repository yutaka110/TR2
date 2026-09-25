"""Corrupt copies of real logs to challenge state provenance and mask audits."""
import argparse,copy
from pathlib import Path
from unittest.mock import patch
import verify_reach_baseline as target
import audit_reach_ablation_state as state
from verify_reach_closed_loop import rows,save

def main():
    p=argparse.ArgumentParser();p.add_argument('b2',type=Path);p.add_argument('b3',type=Path);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    for s in (a.b2,a.b3):target.baseline_audit(s)
    cases=[('B2_task_leak',a.b2,'baseline_state.csv','stage',lambda r:3),
           ('B3_decoder_leak',a.b3,'baseline_state.csv','generation',lambda r:999),
           ('future_publication',a.b2,'baseline_decisions.csv','state_id',lambda r:999999),
           ('fabricated_sequence',a.b2,'baseline_state.csv','sequence',lambda r:999999),
           ('future_receipt',a.b3,'baseline_state.csv','received_us',lambda r:int(r['sampled_us'])+1),
           ('invented_observation',a.b3,'baseline_state.csv','observation_capture_us',lambda r:1),
           ('state_value_forgery',a.b2,'baseline_decisions.csv','state_value',lambda r:3.7),
           ('stale_state_flag',a.b3,'baseline_decisions.csv','state_live',lambda r:1-int(r['state_live'])),
           ('local_IDR_forgery',a.b2,'baseline_decisions.csv','idr',lambda r:1-int(r['idr'])),
           ('false_counterfactual',a.b3,'baseline_decisions.csv','b1_selected',lambda r:99)]
    result=dict(passed=False,checks=[])
    for name,s,file,key,mutate in cases:
        changed=copy.deepcopy(rows(s/file));row=changed[len(changed)//2];row[key]=str(mutate(row))
        def load(path):return changed if path==s/file else rows(path)
        try:
            with patch.object(target,'rows',side_effect=load),patch.object(state,'rows',side_effect=load):target.baseline_audit(s)
        except Exception as e:result['checks'].append(dict(name=name,rejected=True,reason=str(e)))
        else:result['checks'].append(dict(name=name,rejected=False))
    result['passed']=all(c['rejected'] for c in result['checks']);save(a.output,result);print(result);return 0 if result['passed'] else 1

if __name__=='__main__':raise SystemExit(main())
