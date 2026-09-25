"""Reject incorrect time-state/delay/loss evidence without modifying source logs."""
import argparse
from pathlib import Path
from unittest.mock import patch
import verify_reach_link as v


def main():
    parser=argparse.ArgumentParser();parser.add_argument('report',type=Path);args=parser.parse_args()
    session=Path(v.read(args.report)['cases'][0]['session']);original=v.rows;checks=[]
    v.link_audit(session,'uplink');checks.append(dict(name='real_evidence',passed=True))
    faults=[('wrong_interval','delivered','trace_index',1),('wrong_state_time','delivered','trace_at_us',1),('wrong_delay','delivered','delay_us',1),('wrong_due','delivered','scheduled_delivery_us',1)]
    for name,event,key,delta in faults:
        def changed(path):
            data=original(path)
            if path.name=='uplink_link.csv':
                r=next(x for x in data if x['event']==event);r[key]=str(int(r[key])+delta)
            return data
        with patch.object(v,'rows',side_effect=changed):
            try:v.link_audit(session,'uplink')
            except AssertionError as e:checks.append(dict(name=name,passed=True,rejection=str(e)))
            else:checks.append(dict(name=name,passed=False))
    for name in ('early_send','false_loss'):
        def changed(path):
            data=original(path)
            if path.name=='uplink_link.csv':
                r=next(x for x in data if x['event']=='delivered')
                if name=='early_send':r['actual_send_us']=str(int(r['scheduled_delivery_us'])-1)
                else:r['event']='trace_drop'
            return data
        with patch.object(v,'rows',side_effect=changed):
            try:v.link_audit(session,'uplink')
            except AssertionError as e:checks.append(dict(name=name,passed=True,rejection=str(e)))
            else:checks.append(dict(name=name,passed=False))
    result=dict(passed=all(c['passed'] for c in checks),checks=checks)
    v.save(args.report.parent/'trace_auditor_faults.json',result);print(result);return 0 if result['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
