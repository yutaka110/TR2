"""Reject plausible forged success reports without altering original evidence."""
import argparse
import copy
import json
from pathlib import Path
from unittest.mock import patch
import verify_reach_decode as target


def main():
    p=argparse.ArgumentParser();p.add_argument('report',type=Path);args=p.parse_args()
    report=target.read(args.report);target.require(report['passed'],'input acceptance failed')
    normal=Path(report['cases'][0]['session']);missing=Path(next(c for c in report['cases'] if c['name']=='p_missing')['session'])
    def change(event,key,value):
        def mutate(data):next(r for r in data if r['event']==event).__setitem__(key,value)
        return mutate
    mutations=[('borrow_generation',normal,'decode_events.csv',change('decoded_output','generation','999')),
               ('forge_output_capture',normal,'decode_events.csv',change('decoded_output','capture_us','1')),
               ('forge_state',normal,'decode_events.csv',change('reassembled','state','Synchronized')),
               ('forge_display_deadline',normal,'decode_events.csv',change('display_adopted','display_deadline_us','0')),
               ('forge_control_use',normal,'decode_events.csv',change('control_rejected','event','control_used')),
               ('hide_flush_loss',missing,'decode_events.csv',change('input_abandoned','event','ignored')),
               ('forge_missing_chunks',normal,'reassembly_events.csv',change('completed','missing_chunks','1')),
               ('fake_idr',normal,'decode_events.csv',change('input_accepted','idr','0'))]
    result=dict(passed=False,checks=[]);original=target.rows
    for name,session,file,mutation in mutations:
        data=copy.deepcopy(original(session/file));mutation(data)
        def load(path):return data if Path(path)==session/file else original(path)
        try:
            with patch.object(target,'rows',side_effect=load):target.decode_audit(session)
        except Exception as error:result['checks'].append(dict(name=name,rejected=True,reason=str(error)))
        else:result['checks'].append(dict(name=name,rejected=False))
    result['passed']=all(c['rejected'] for c in result['checks'])
    target.save(args.report.parent/'auditor_fault_tests.json',result)
    print(json.dumps(result,indent=2));return 0 if result['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
