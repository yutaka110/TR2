"""Replay masked state from accepted RSTA and sender estimate publication prefixes."""
from collections import Counter
from verify_reach_closed_loop import read, rows
from verify_reach_command import require
from verify_reach_state import packet


FIELDS=('estimate_id','sampled_us','received_us','generated_us','sequence','mask','reference','generation','decode_wait_us','stage','observation_capture_us','position_error_micro','deadline_us')


def state_value(s,now,idr):
    live=bool(s.get('sequence',0) and s['received_us']<=s['sampled_us']<=now and s['generated_us']<=s['received_us'] and now-s['generated_us']<250000)
    if not live or not s['mask']:return 1.,live
    if s['mask']==1:
        sync=s['reference']==1 and s['generation']>0
        return (1. if sync else 2. if idr else .15)*(1-.5*min(1.,s['decode_wait_us']/200000)),live
    age=now-s['observation_capture_us'] if 0<s['observation_capture_us']<=now else 200000
    uncertainty=min(1.,(s['position_error_micro']+.3*age)/100000) if s['observation_capture_us'] else 1.
    urgency=.5 if 0<s['deadline_us']-now<=5000000 else 0.
    return min(4.,(2.,1.,1.5,2.)[s['stage']]+min(1.,age/200000)+uncertainty+urgency),live


def state_inputs(session,mode):
    published=rows(session/'baseline_state.csv');estimates=rows(session/'sender_estimates.csv')
    accepted={}
    for r in rows(session/'state_rx.csv'):
        if r['status']=='accepted':
            p,_=packet(r['wire_hex']);accepted[p['sequence']]=(p,int(r['received_us']))
    if mode in ('B0','B1'):
        require(not published,'masked-off state reached baseline');return [(0,{})]
    require(len(published)==len(estimates),'state publication partition')
    result=[(0,{})]
    for index,(r,e) in enumerate(zip(published,estimates),1):
        got={k:int(r[k]) for k in FIELDS};seq=int(e['accepted_sequence']);p,receipt=accepted[seq] if seq else ({},0)
        want=dict.fromkeys(FIELDS,0)
        want.update(estimate_id=int(e['estimate_id']),sampled_us=int(e['event_us']),received_us=receipt,
                    generated_us=p.get('generated_us',0),sequence=seq,mask=1 if mode=='B2' else 2)
        require(receipt<=want['sampled_us'],'publication borrowed future packet')
        if mode=='B2':want.update(reference=p.get('reference',0),generation=p.get('generation',0),decode_wait_us=p.get('decode_wait_us',0))
        else:want.update(stage=p.get('task_stage',0),observation_capture_us=p.get('observation_capture_us',0),
                         position_error_micro=p.get('position_error_micro',0),deadline_us=p.get('task_deadline_us',0))
        require(got==want,'state projection differs from received masked fields')
        now=int(r['published_us']);require(int(r['state_id'])==index and now>=want['sampled_us'] and now>=result[-1][0],'publication order')
        result.append((now,got))
    return result


def audit_state_decision(d,encoded,states):
    now=int(d['event_us']);prefix=int(d['state_id']);require(0<=prefix<len(states),'unknown state publication')
    published,s=states[prefix];require(published<=now,'future state publication')
    if prefix+1<len(states):require(states[prefix+1][0]>=now,'not latest atomic publication prefix')
    require(d['idr']==encoded['idr'],'local AU identity changed')
    value,live=state_value(s,now,d['idr']=='1')
    if d['probe']=='1':value=1.
    require(int(d['state_live'])==int(live),'stale publication used as current')
    require(abs(float(d['state_value'])-value)<1e-9,'state value mismatch')


def ablation_summary(session):
    ds=rows(session/'baseline_decisions.csv');mode=read(session/'config.effective.json')['baseline']['mode']
    changed=[d for d in ds if d['selected']!=d['b1_selected']]
    return dict(mode=mode,decisions=len(ds),live_state_decisions=sum(d['state_live']=='1' for d in ds),
                value_changed=sum(abs(float(d['state_value'])-1)>1e-9 for d in ds),
                action_changed_against_same_input_B1=len(changed),
                changes=dict(Counter(d['b1_selected']+'->'+d['selected'] for d in changed)),
                scope='one-step counterfactual at identical local inputs and lambda; not a counterfactual task trajectory')
