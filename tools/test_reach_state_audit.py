"""Prove the G2-05 auditor rejects hidden/future state and false freshness."""
import argparse
import copy
from pathlib import Path
from unittest.mock import patch
import verify_reach_state as target

def main():
    p=argparse.ArgumentParser();p.add_argument('report',type=Path);args=p.parse_args();r=target.read(args.report)
    target.require(r['passed'],'acceptance incomplete');normal=Path(r['cases'][0]['session']);none=Path(next(c for c in r['cases'] if c['name']=='no_notification')['session']);blackout=Path(next(c for c in r['cases'] if c['name']=='blackout')['session'])
    def edit(predicate,key,value):
        def mutate(data):row=next(x for x in data if predicate(x));row[key]=str(value(row) if callable(value) else value)
        return mutate
    accepted=lambda x:int(x['accepted_sequence'])>0
    stale=lambda x:x['decision']=='notification_stale'
    cases=[('unreceived_state',none,'sender_estimates.csv',edit(lambda x:True,'decoded_frame',99)),
           ('borrow_future_report',normal,'sender_estimates.csv',edit(accepted,'accepted_sequence',lambda x:int(x['accepted_sequence'])+1)),
           ('forge_receiver_pose',normal,'sender_estimates.csv',edit(accepted,'x_micro',99999999)),
           ('rebase_age_at_receipt',normal,'sender_estimates.csv',edit(accepted,'notification_age_us',0)),
           ('revive_stale_reference',blackout,'sender_estimates.csv',edit(stale,'reference_reported_synchronized',1)),
           ('hide_uncertainty_growth',blackout,'sender_estimates.csv',edit(stale,'position_radius_micro',0)),
           ('accept_invalid_packet',normal,'state_rx.csv',edit(lambda x:True,'status','duplicate')),
           ('receive_before_delivery',normal,'state_rx.csv',edit(lambda x:True,'received_us',1))]
    original=target.rows;result=dict(passed=False,checks=[])
    for name,session,file,mutation in cases:
        data=copy.deepcopy(original(session/file));mutation(data)
        def load(path):return data if Path(path)==session/file else original(path)
        try:
            with patch.object(target,'rows',side_effect=load):target.state_audit(session)
        except Exception as error:result['checks'].append(dict(name=name,rejected=True,reason=str(error)))
        else:result['checks'].append(dict(name=name,rejected=False))
    result['passed']=all(c['rejected'] for c in result['checks']);target.save(args.report.parent/'auditor_fault_tests.json',result);print(result)
    return 0 if result['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
