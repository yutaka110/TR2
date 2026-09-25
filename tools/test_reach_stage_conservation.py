"""Reject missing terminal records or unsupported inferred jitter discards."""
import argparse
from pathlib import Path
from unittest.mock import patch
import audit_reach_stage_conservation as target
from verify_reach_closed_loop import read, save


def main():
    p=argparse.ArgumentParser();p.add_argument('report',type=Path);args=p.parse_args();report=read(args.report)
    original=target.rows; checks=[]
    for name,case_name,kind in [('missing_old_frame_witness','T2_congestion','decode_dequeued'),('missing_input_rejection','T2_congestion','input_rejected'),('missing_abandonment','T1_seeded_mixed','input_abandoned')]:
        session=Path(next(c['session'] for c in report['cases'] if c['name']==case_name));target.stage_conservation(session)
        events=[e for e in original(session/'decode_events.csv') if e['event']!=kind]
        def load(path):return events if path==session/'decode_events.csv' else original(path)
        try:
            with patch.object(target,'rows',side_effect=load):target.stage_conservation(session)
        except AssertionError as error:checks.append(dict(name=name,rejected=True,reason=str(error)))
        else:checks.append(dict(name=name,rejected=False))
    result=dict(passed=all(c['rejected'] for c in checks),checks=checks);save(args.report.parent/'stage_conservation_fault_tests.json',result);print(result)
    return 0 if result['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
