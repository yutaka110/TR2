"""Fault injection in memory; preserve all original capacity-link evidence."""
import argparse
import copy
from pathlib import Path
from unittest.mock import patch
import verify_reach_link as v


def main():
    parser=argparse.ArgumentParser();parser.add_argument('report',type=Path);args=parser.parse_args()
    case=v.read(args.report)['cases'][0];session=Path(case['session']);original=v.rows;checks=[]
    v.link_audit(session,'uplink');checks.append(dict(name='real_evidence',passed=True))
    faults=[('early_completion','delivered','completion_us',-1),('wrong_service_start','delivered','start_us',1),('wrong_occupancy','admitted','occupied_ip_bytes',1)]
    for name,event,key,delta in faults:
        def corrupted(path):
            data=original(path)
            if path.name=='uplink_link.csv':
                r=next(x for x in data if x['event']==event);r[key]=str(int(r[key])+delta)
            return data
        with patch.object(v,'rows',side_effect=corrupted):
            try:v.link_audit(session,'uplink')
            except AssertionError as e:checks.append(dict(name=name,passed=True,rejection=str(e)))
            else:checks.append(dict(name=name,passed=False))
    def tail_drop(path):
        data=original(path)
        if path.name=='uplink_link.csv':next(x for x in data if x['event']=='admitted')['event']='tail_drop'
        return data
    with patch.object(v,'rows',side_effect=tail_drop):
        try:v.link_audit(session,'uplink')
        except AssertionError as e:checks.append(dict(name='false_tail_drop',passed=True,rejection=str(e)))
        else:checks.append(dict(name='false_tail_drop',passed=False))
    report=dict(passed=all(x['passed'] for x in checks),checks=checks,scope='in-memory edits only; original records untouched')
    v.save(args.report.parent/'auditor_fault_tests.json',report);print(report);return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
