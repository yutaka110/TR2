"""Mutation tests for cross-stage provenance and independent outcome replay."""
import argparse
import copy
from pathlib import Path
from unittest.mock import patch
import verify_reach_integration as target


def main():
    parser = argparse.ArgumentParser(); parser.add_argument('session',type=Path); parser.add_argument('--output',type=Path,required=True); args = parser.parse_args()
    baseline = target.integration_audit(args.session)
    def edit(predicate, key, value):
        def mutate(data):
            row = next(r for r in data if predicate(r)); row[key] = str(value(row) if callable(value) else value)
        return mutate
    cases = [
        ('future_recognition','decode_events.csv',edit(lambda r:r['event']=='recognition_accepted','event_us',999999999999999)),
        ('truth_instead_of_image_command','commands.csv',edit(lambda r:float(r['v_m_s'])>.1,'v_m_s',.299123)),
        ('invented_world_motion','world.csv',edit(lambda r:int(r['physics_tick'])==25,'x_m',.7)),
        ('invented_success','world.csv',edit(lambda r:int(r['physics_tick'])==25,'success',1)),
        ('wrong_capture_world','capture_evaluation.csv',edit(lambda r:True,'y_m',.7)),
        ('reassembly_before_delivery','decode_events.csv',edit(lambda r:r['event']=='reassembled','event_us',1)),
        ('actuation_borrowed_image','udp_applied_commands.csv',edit(lambda r:int(r['source_frame_id'])>0,'source_frame_id',999999)),
        ('actuation_before_acceptance','udp_applied_commands.csv',edit(lambda r:int(r['source_frame_id'])>0,'applied_us',1)),
        ('receive_before_relay','command_rx.csv',edit(lambda r:r['status']=='accepted','received_us',1)),
        ('wrong_task_phase','commands.csv',edit(lambda r:r['state']=='APPROACH','state','HOLD')),
    ]
    result = dict(passed=False,baseline=baseline,checks=[]); original = target.rows
    for name, filename, mutate in cases:
        path = args.session/filename; data = copy.deepcopy(original(path)); mutate(data)
        def load(candidate): return data if candidate == path else original(candidate)
        try:
            with patch.object(target,'rows',side_effect=load): target.integration_audit(args.session)
        except AssertionError as error: result['checks'].append(dict(name=name,rejected=True,reason=str(error)))
        else: result['checks'].append(dict(name=name,rejected=False))
    result['passed'] = all(c['rejected'] for c in result['checks']); target.save(args.output,result)
    print(result['checks']); return 0 if result['passed'] else 1


if __name__ == '__main__': raise SystemExit(main())
