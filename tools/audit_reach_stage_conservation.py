"""Account for every captured frame, including old completed jitter-buffer inputs.

Older-than-released discard is inferred from a prior *observed* decode dequeue
and the fixed JitterBuffer::PushFrame guard, not labelled a native drop event.
"""
from collections import Counter, defaultdict
from verify_reach_closed_loop import rows
from verify_reach_command import require


def stage_conservation(session):
    events = rows(session/'decode_events.csv'); captures = rows(session/'captures.csv')
    frames = defaultdict(dict); dequeues = []
    for e in events:
        frames[int(e['frame_id'])][e['event']] = e
        if e['event'] == 'decode_dequeued': dequeues.append(e)
    result = []; counts = Counter()
    for capture in captures:
        fid = int(capture['frame_id']); f = frames[fid]; witness = None
        if 'completed_rejected' in f:require('reassembled' in f and 'decode_dequeued' not in f,'contradictory completed rejection')
        if 'reassembled' not in f:
            outcome = 'transport_unassembled'; reason = 'no_completed_AU_in_full_run_and_drain'
        elif 'decode_dequeued' not in f:
            complete = f['reassembled']
            rejected=f.get('completed_rejected')
            if rejected:
                prefix='older_than_jitter_release:';require(rejected['reason'].startswith(prefix),'unknown completed rejection')
                wid=int(rejected['reason'][len(prefix):]);witness=frames[wid].get('reassembled')
                require(wid>fid and witness and witness['stream_id']==complete['stream_id']==rejected['stream_id'],'invalid jitter rejection witness')
                require(int(witness['event_us'])<=int(rejected['event_us']) and int(complete['event_us'])<=int(rejected['event_us']),'future jitter rejection witness')
                outcome='observed_older_than_released';reason='native_jitter_guard_recorded_last_released_frame'
            else:
                witness = next((d for d in dequeues if d['stream_id'] == complete['stream_id'] and int(d['frame_id']) > fid and int(d['event_us']) < int(complete['event_us'])), None)
                require(witness is not None, 'unexplained completed frame without decoder dequeue')
                outcome = 'inferred_older_than_released'; reason = 'prior_newer_dequeue_proves_JitterBuffer_PushFrame_old_frame_guard'
        elif 'input_accepted' not in f:
            require('input_rejected' in f, 'dequeued frame lacks acceptance/rejection')
            outcome = 'decoder_input_rejected'; reason = f['input_rejected']['reason']
        elif 'decoded_output' not in f:
            require('input_abandoned' in f, 'accepted input has no output or abandonment')
            outcome = 'input_abandoned'; reason = f['input_abandoned']['reason']
        else:
            outcome = 'decoded_pixels'; reason = 'actual_pixel_output'
        counts[outcome] += 1
        result.append(dict(frame_id=fid,outcome=outcome,reason=reason,
            recognition='accepted' if 'recognition_accepted' in f else 'rejected' if 'recognition_rejected' in f else 'none',
            completed_us=f.get('reassembled',{}).get('event_us',''),
            witness_newer_frame=witness['frame_id'] if witness else '',
            witness_dequeue_us=witness['event_us'] if witness and outcome=='inferred_older_than_released' else '',
            witness_complete_us=witness['event_us'] if witness and outcome=='observed_older_than_released' else ''))
    require(sum(counts.values()) == len(captures), 'frame terminal partition')
    return dict(passed=True,counts=dict(counts),frames=result)
