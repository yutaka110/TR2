"""Independent G3-01 audit: wire feedback prefixes, finite plans and real actions.

The evaluator can inspect truth AFTER a trial. The native planner never imports it.
RTT/loss are numerically replayed; native BWE/local pacer inputs are bounds checked,
not independently reimplemented. Shared budget/link audits validate actual costs.
"""
from collections import Counter, defaultdict
import math
from verify_reach_closed_loop import read, rows
from verify_reach_command import require


def close(a, b, message):
    require(math.isfinite(float(a)) and math.isclose(float(a), float(b), rel_tol=2e-9, abs_tol=2e-9), message)


def ip_size(size, group):
    chunks = (size+1199)//1200
    parity = sum(min(group, chunks-i)>1 for i in range(0, chunks, group)) if group else 0
    return size+chunks*72+parity*1280


def candidates(d):
    size = int(d['payload_bytes']); count = (size+1199)//1200
    loss = float(d['loss']); burst = float(d['burst']); weight = float(d['effective_lambda'])
    p = min(.75, max(.00001, loss+.25*max(0, burst-loss)))
    result = []
    for group in (0,2,4,8):
        for rounds in range(3):
            initial = ip_size(size, group)
            expected = initial+sum(ip_size(size,0)*p**j for j in range(1,rounds+1))
            finish = int(d['event_us'])+int(d['queue_us'])+int(d['rtt_us'])//2+8000+math.ceil(expected*8000000/int(d['rate_bps']))+rounds*int(d['rtt_us'])
            feasible = finish <= int(d['capture_us'])+200000
            residual = p**(rounds+1)
            sizes = [min(group,count-i) for i in range(0,count,group)] if group else [count]
            success = math.prod((1-residual)**n+(n*residual*(1-residual)**(n-1)*(1-p) if group and n>1 else 0) for n in sizes) if feasible else 0
            result.append(dict(group=group, rounds=rounds, initial_ip=initial, expected_ip=expected, success=success,
                               cost=1-success+weight*expected/ip_size(size,2), feasible=int(feasible), defer=0))
    result.append(dict(group=0, rounds=0, initial_ip=0, expected_ip=0, success=0, cost=1, feasible=1, defer=1))
    value=float(d.get('state_value',1))
    if value!=1:
        for c in result:c['cost']=value if c['defer'] else value*(1-c['success'])+weight*c['expected_ip']/ip_size(size,2)
    return result


def baseline_audit(session):
    config = read(session/'config.effective.json'); mode = config['baseline']['mode']; model = read(session/'baseline_model.json')
    require(mode in ('B0','B1','B2','B3') and model['mode']==mode, 'baseline mode mismatch')
    ablation=model['version']=='G3-02-v1'
    require(ablation or model['version']=='G3-01-v3','pre-probing baseline is not qualified')
    close(config['baseline']['lambda'],model['lambda'],'model lambda mismatch')
    require(model['reference_task_inputs'] is ablation and model['network_trace_input'] is False, 'forbidden input enabled')
    if ablation:
        from audit_reach_ablation_state import state_inputs,audit_state_decision
        state_prefixes=state_inputs(session,mode)
    fbs = rows(session/'baseline_feedback.csv'); actual = rows(session/'feedback_rx.csv')
    require(len(fbs)==len(actual),'missing feedback journal')
    loss = burst = .01; rtt = 40000.; last = nacks = 0; missing_before = False
    snapshots = [(0,loss,1.,int(rtt))]
    for i,(f,a) in enumerate(zip(fbs,actual),1):
        now = int(f['received_us']); b = bytes.fromhex(f['wire_hex']); payload = b[44:]
        require(int(f['event_id'])==i and f['wire_hex']==a['wire_hex'] and now>=int(a['received_us']) and now>=snapshots[-1][0], 'feedback wire/order mismatch')
        require(b[:4]==b'RNVP' and len(b)==44+int.from_bytes(b[32:36],'big'),'non-RNVP observation')
        if b[5]==3:
            sent = int.from_bytes(payload[:8],'big')
            if sent and sent<=now:rtt=.875*rtt+.125*(now-sent)
        elif b[5]==1:
            if int.from_bytes(payload[8:12],'big'):nacks+=1
        elif b[5]==5:
            base = int.from_bytes(payload[:4],'big'); count = int.from_bytes(payload[4:6],'big')
            require(len(payload)==16+count*8,'transport feedback length')
            for j in range(count):
                entry = payload[16+j*8:24+j*8]; seq=base+int.from_bytes(entry[:2],'big'); flags=entry[2]
                if seq<=last or not flags&3:continue
                missing=bool(flags&2); loss=.95*loss+.05*missing;burst=.95*burst+.05*(missing and missing_before)
                missing_before=missing;last=seq
        close(f['loss'],loss,'feedback loss replay');close(f['burst'],burst,'feedback burst replay');close(f['rtt_us'],rtt,'PONG RTT replay')
        require(int(f['last_sequence'])==last and int(f['nacks'])==nacks,'feedback counters')
        snapshots.append((now,loss,min(1.,max(0.,burst/loss)) if loss>1e-6 else 0.,int(rtt)))
    encoded = {int(e['frame_id']):e for e in rows(session/'encoded.csv')}
    ds = rows(session/'baseline_decisions.csv'); byid = {int(d['frame_id']):d for d in ds}
    require(len(ds)==len(byid)==len(encoded) and set(byid)==set(encoded),'decision/encoded partition')
    cs = defaultdict(list)
    for c in rows(session/'baseline_candidates.csv'):cs[int(c['frame_id'])].append(c)
    require(set(cs)==set(byid) if mode!='B0' else not cs,'candidate frame partition')
    previous = 0; previous_offer = 0; actions = Counter(); timings = []
    for d in ds:
        fid = int(d['frame_id']); e=encoded[fid]; now=int(d['event_us']); prefix=int(d['feedback_id'])
        require(0<=prefix<len(snapshots),'unknown feedback prefix')
        snap=snapshots[prefix]
        require(now>=previous and int(e['encoder_output_us'])<=now and snap[0]<=now and int(d['last_feedback_us'])==snap[0],'future feedback or local timestamp')
        previous=now
        if prefix+1<len(snapshots):require(snapshots[prefix+1][0]>=now,'not the logged atomic feedback prefix')
        close(d['loss'],snap[1],'decision loss prefix');close(d['burst'],snap[2],'decision burst prefix')
        require(int(d['rtt_us'])==snap[3],'decision RTT prefix')
        require(int(d['payload_bytes'])==int(e['bytes'])+56 and d['capture_us']==e['capture_us'],'AU size/clock provenance')
        require(100000<=int(d['rate_bps'])<=min(config['ip_budget'][k]['rate_bps'] for k in ('total','uplink')) and int(d['queue_us'])>=0,'BWE/pacer bounds')
        rate=int(d['rate_bps']);require(int(d['queue_us'])==(int(d['queued_payload_bytes'])*8000000+rate-1)//rate,'queue estimate must use live backlog')
        close(d['lambda'],config['baseline']['lambda'],'decision lambda changed')
        started=int(d['decision_start_us']);require(previous_offer<=started<=now and int(d['previous_offer_us'])==previous_offer,'local offer clock/history')
        probe=mode!='B0' and started-previous_offer>=500000
        require(int(d['probe'])==int(probe),'idle probe eligibility')
        close(d['effective_lambda'],0 if probe else config['baseline']['lambda'],'probe cost rule')
        if probe:require(rate==model['pacing_ceiling_bps']//1000*1000,'probe must use known budget ceiling')
        if d['defer']=='0':previous_offer=now
        if ablation:audit_state_decision(d,e,state_prefixes)
        if mode!='B0':
            expected=candidates(d); logged=cs[fid]; require(len(logged)==13,'candidate count')
            for index,(got,want) in enumerate(zip(logged,expected)):
                require(int(got['candidate'])==index,'candidate ordering')
                for key,value in want.items():close(got[key],value,'candidate mismatch '+key)
            best=12
            for index,c in enumerate(expected):
                if c['feasible'] and c['cost']<expected[best]['cost']-1e-12:best=index
            require(int(d['selected'])==best,'nonminimal action')
            if ablation:
                plain=candidates(dict(d,state_value=1));b1best=12
                for index,c in enumerate(plain):
                    if c['feasible'] and c['cost']<plain[b1best]['cost']-1e-12:b1best=index
                require(int(d['b1_selected'])==b1best,'same-input B1 counterfactual wrong')
            for key in ('group','rounds','defer'):require(int(d[key])==expected[best][key],'chosen action mismatch')
        else:
            require(d['defer']=='0' and d['rounds']=='3' and int(d['group']) in (0,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16),'B0 changed fixed mechanism')
        actions[f"g{d['group']}/r{d['rounds']}/defer{d['defer']}"]+=1;timings.append(int(d['decision_us']))
    # Match actual initial video/FEC packets to the selected frame action.
    # Offered packets may be dropped by the common budget or pacer, so no false
    # equality between theoretical bytes and actual attempts is imposed.
    parity_checked = data_checked = 0
    for packet in rows(session/'ip_budget.csv'):
        if packet['direction']!='uplink':continue
        b=bytes.fromhex(packet['wire_hex']);typ=b[5];flags=int.from_bytes(b[36:40],'big')
        if typ not in (0,6):continue
        fid=int.from_bytes(b[16:20],'big');require(fid in byid,'packet without frame decision');d=byid[fid]
        require(d['defer']=='0','deferred frame transmitted')
        if typ==0 and not flags&16:data_checked+=1
        if mode!='B0' and typ==6 and not flags&16:
            group=int(d['group']);start=int.from_bytes(b[20:22],'big');chunks=int.from_bytes(b[22:24],'big')
            require(group>0 and start%group==0 and int.from_bytes(b[50:52],'big')==min(group,chunks-start),'actual XOR group differs from plan')
            parity_checked+=1
    counts=Counter();repair_rows=rows(session/'baseline_repairs.csv')
    for r in repair_rows:
        fid=int(r['frame_id']);require(fid in byid,'repair frame absent');d=byid[fid];counts[fid]+=1
        prefix=int(r['feedback_id']);require(0<=prefix<len(snapshots),'repair prefix')
        require(snapshots[prefix][0]<=int(r['event_us']) and snapshots[prefix][3]==int(r['rtt_us']),'repair future RTT')
        require(int(r['repair_request'])==counts[fid] and r['capture_us']==d['capture_us'] and r['rounds']==d['rounds'],'repair local history')
        rate=int(r['queue_rate_bps']);require(rate>=100000 and int(r['queue_us'])==(int(r['queued_payload_bytes'])*8000000+rate-1)//rate,'repair live queue estimate')
        allowed=mode=='B0' or (counts[fid]<=int(d['rounds']) and int(r['event_us'])+int(r['queue_us'])+int(r['rtt_us'])//2+8000<=int(d['capture_us'])+200000)
        require(int(r['allowed'])==int(allowed),'repair gate mismatch')
    timings.sort()
    return dict(passed=True,mode=mode,decisions=len(ds),feedback_received=len(fbs),candidate_rows=sum(map(len,cs.values())),
                actual_initial_data_packets=data_checked,actual_initial_fec_packets_checked=parity_checked,actions=dict(actions),
                repair_requests=len(repair_rows),repair_requests_allowed=sum(r['allowed']=='1' for r in repair_rows),
                decision_us_p95=timings[math.ceil(.95*len(timings))-1],decision_us_max=max(timings),
                empty_queue_with_historical_delay=sum(int(d['queued_payload_bytes'])==0 and int(d['historical_queue_us'])>0 for d in ds),
                idle_probe_decisions=sum(d['probe']=='1' for d in ds),idle_probe_offers=sum(d['probe']=='1' and d['defer']=='0' for d in ds),
                limitation='RTT/loss/plan/actual FEC replay; native BWE and pacer measured inputs are bounded, not independently replayed; repair-cache misses use B0 native/B1 reject fallback')
