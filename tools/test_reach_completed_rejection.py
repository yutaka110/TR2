"""Regression: jitter release can precede both old arrival and later decoder dequeue."""
import argparse,copy
from pathlib import Path
from unittest.mock import patch
import audit_reach_stage_conservation as target
from verify_reach_closed_loop import save

def main():
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    def e(fid,kind,time,reason=''):return dict(frame_id=str(fid),stream_id='7',event=kind,event_us=str(time),reason=reason)
    original=[e(421,'reassembled',100),e(420,'reassembled',110),e(420,'completed_rejected',120,'older_than_jitter_release:421'),e(421,'decode_dequeued',200),e(421,'input_rejected',210,'awaiting_idr')]
    captures=[dict(frame_id='420'),dict(frame_id='421')]
    def run(events):
        with patch.object(target,'rows',side_effect=lambda p:events if p.name=='decode_events.csv' else captures):return target.stage_conservation(Path('.'))
    good=run(original);assert good['counts']==dict(observed_older_than_released=1,decoder_input_rejected=1)
    cases=[]
    missing=[r for r in original if r['event']!='completed_rejected'];cases.append(('missing_guard',missing))
    for name,key,value in [('unknown_witness','reason','older_than_jitter_release:999'),('older_witness','reason','older_than_jitter_release:419'),('future_witness','event_us','90'),('wrong_stream','stream_id','9')]:
        changed=copy.deepcopy(original);changed[2][key]=value;cases.append((name,changed))
    results=[]
    for name,events in cases:
        try:run(events)
        except AssertionError as exc:results.append(dict(name=name,rejected=True,reason=str(exc)))
        else:results.append(dict(name=name,rejected=False))
    report=dict(passed=all(r['rejected'] for r in results),valid_release_before_dequeue=True,checks=results);a.output.parent.mkdir(parents=True,exist_ok=True);save(a.output,report);print(report)
    return 0 if report['passed'] else 1

if __name__=='__main__':raise SystemExit(main())
