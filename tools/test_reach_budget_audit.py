"""In-memory evidence corruption; never modify the original experiment logs."""
import argparse
from pathlib import Path
from unittest.mock import patch
import verify_reach_budget as v


def main():
    p=argparse.ArgumentParser();p.add_argument('report',type=Path);args=p.parse_args()
    report=v.read(args.report);session=Path(report['cases'][0]['session']);original=v.rows;checks=[]
    v.budget_audit(session);checks.append(dict(name='real_evidence',passed=True))
    for name in ('exclude_IP_header','FEC_as_video','wrong_total','charge_rejected','bypass_gate','lose_feedback'):
        target=Path(report['cases'][1]['session']) if name=='charge_rejected' else session
        def changed(path):
            data=original(path)
            if path.name=='ip_budget.csv':
                r=next(x for x in data if x['status']=='sent')
                if name=='exclude_IP_header':r['ip_bytes']=r['payload_bytes']
                if name=='FEC_as_video':next(x for x in data if x['kind']=='fec')['kind']='video_delta'
                if name=='wrong_total':r['total_used_ip_bytes']=str(int(r['total_used_ip_bytes'])+1)
                if name=='charge_rejected':
                    r=next(x for x in data if x['status']=='total_byte_limit');r['attempted_ip_bytes']=r['ip_bytes']
                if name=='bypass_gate':r['status']='total_byte_limit'
            if path.name=='feedback_rx.csv' and name=='lose_feedback':data.pop(0)
            return data
        with patch.object(v,'rows',side_effect=changed):
            try:v.budget_audit(target)
            except AssertionError as e:checks.append(dict(name=name,passed=True,rejection=str(e)))
            else:checks.append(dict(name=name,passed=False))
    result=dict(passed=all(c['passed'] for c in checks),checks=checks);v.save(args.report.parent/'auditor_fault_tests.json',result);print(result);return 0 if result['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
