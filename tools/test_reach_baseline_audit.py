"""Negative tests against real baseline evidence; never alter the saved logs."""
import argparse
import copy
from pathlib import Path
from unittest.mock import patch
import verify_reach_baseline as target
from verify_reach_closed_loop import save


def main():
    p=argparse.ArgumentParser();p.add_argument('session',type=Path);p.add_argument('--output',type=Path,required=True);args=p.parse_args()
    target.baseline_audit(args.session)
    original=target.rows
    def edit(predicate,key,value):
        def mutate(data):
            row=next(r for r in data if predicate(r));row[key]=str(value(row) if callable(value) else value)
        return mutate
    cases=[('hide_idle_probe','baseline_decisions.csv',edit(lambda r:r['probe']=='1','probe',0)),
           ('stale_queue_delay','baseline_decisions.csv',edit(lambda r:int(r['queued_payload_bytes'])==0 and int(r['historical_queue_us'])>0,'queue_us',lambda r:r['historical_queue_us'])),
           ('forge_loss','baseline_decisions.csv',edit(lambda r:True,'loss',.9)),
           ('borrow_future_feedback','baseline_decisions.csv',edit(lambda r:True,'feedback_id',len(original(args.session/'baseline_feedback.csv')))),
           ('forge_rtt','baseline_decisions.csv',edit(lambda r:True,'rtt_us',1)),
           ('hide_AU_bytes','baseline_decisions.csv',edit(lambda r:True,'payload_bytes',100)),
           ('nonminimal_action','baseline_decisions.csv',edit(lambda r:r['selected']!='12','selected',12)),
           ('forge_candidate_value','baseline_candidates.csv',edit(lambda r:True,'success',.99999)),
           ('hide_feedback','baseline_feedback.csv',lambda data:data.pop()),
           ('reverse_repair_gate','baseline_repairs.csv',edit(lambda r:True,'allowed',lambda r:1-int(r['allowed']))),
           ('change_actual_fec_group','ip_budget.csv',edit(lambda r:r['direction']=='uplink' and bytes.fromhex(r['wire_hex'])[5]==6,'wire_hex',lambda r:r['wire_hex'][:100]+'ffff'+r['wire_hex'][104:]))]
    report=dict(passed=False,checks=[])
    for name,file,mutation in cases:
        data=copy.deepcopy(original(args.session/file));mutation(data)
        def load(path):return data if Path(path)==args.session/file else original(path)
        try:
            with patch.object(target,'rows',side_effect=load):target.baseline_audit(args.session)
        except Exception as e:report['checks'].append(dict(name=name,rejected=True,reason=str(e)))
        else:report['checks'].append(dict(name=name,rejected=False))
    report['passed']=all(r['rejected'] for r in report['checks']);save(args.output,report);print(report)
    return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
