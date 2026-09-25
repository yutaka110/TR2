"""Independent causal/action/wire checks and short real-UDP G4-01 acceptance."""
import argparse,copy,hashlib,json,subprocess,sys
from collections import Counter,defaultdict
from pathlib import Path
from unittest.mock import patch
from verify_reach_closed_loop import read,rows,save
from verify_reach_command import require
from verify_reach_budget import budget_audit,packet_class
from verify_reach_decode import decode_audit
from verify_reach_state import state_audit
from verify_reach_integration import integration_audit,direction


def action_audit(session):
    candidates=rows(session/'action_candidates.csv');events=rows(session/'action_events.csv');wire=rows(session/'action_wire.csv')
    encoded={int(r['frame_id']):r for r in rows(session/'encoded.csv')}
    budget=rows(session/'ip_budget.csv');summary=read(session/'ip_budget_summary.json');origin=summary['origin_us']
    groups=defaultdict(list)
    for r in candidates:groups[int(r['decision_id'])].append(r)
    require(list(groups)==list(range(1,len(groups)+1)),'decision sequence')
    selected={};reasons=Counter()
    for key,cs in groups.items():
        require(len(cs)==7 and [c['kind'] for c in cs]==['DEFER','FRESH','PROTECT','PROTECT','PROTECT','REPAIR','REFRESH'],'candidate inventory')
        chosen=[c for c in cs if c['selected']=='1'];require(len(chosen)==1 and chosen[0]['eligible']=='1','illegal selection')
        selected[key]=chosen[0]
        for c in cs:
            kind=c['kind'];now=int(c['event_us']);cap=int(c['capture_us']);size=int(c['payload_bytes']);remaining=int(c['remaining_ip']);g=int(c['group'])
            missing=[int(x) for x in c['missing_chunks'].split(';') if x];require(len(missing)==int(c['missing_count']),'missing count')
            require(c['eligible']==str(int(c['reason']=='eligible')),'eligibility reason mismatch');reasons[c['reason']]+=1
            if kind=='DEFER':require(c['eligible']=='1' and int(c['ip_bytes'])==0,'DEFER not legal');continue
            if kind=='REFRESH':
                legal=c['refresh_wanted']=='1' and c['refresh_pending']=='0' and (int(c['last_refresh_us'])==0 or now-int(c['last_refresh_us'])>=500000)
                require(c['eligible']==str(int(legal)),'refresh pending/cooldown bypass');continue
            if size:
                source=encoded[int(c['frame_id'])]
                require(size==int(source['bytes'])+56 and cap==int(source['capture_us']) and int(source['encoder_output_us'])<=now,'unmaterialized AU')
            n=(size+1199)//1200;ip=size+72*n
            if kind=='PROTECT':ip+=sum(1280 for i in range(0,n,g) if min(g,n-i)>1)
            if kind=='REPAIR':ip=sum(min(1200,size-i*1200)+72 for i in missing if i*1200<size)
            # Invalid chunk requests may be rejected before summing all bytes.
            if c['reason']!='chunk_unavailable':require(int(c['ip_bytes'])==ip,'candidate IP estimate mismatch')
            if c['eligible']=='1':
                require(size>0 and 0<=now-cap<200000 and c['generation']==c['active_generation'] and ip<=remaining,'invalid eligible media')
                if kind in ('FRESH','PROTECT'):require(c['offered']=='0','repeat fresh offer')
                if kind=='PROTECT':require(size>1200,'nonexistent parity')
                if kind=='REPAIR':require(c['offered']=='1' and missing and len(set(missing))==len(missing) and all(i<n for i in missing) and int(c['repair_requests'])<2,'invalid repair')
    feedback=[]
    for r in rows(session/'feedback_rx.csv'):
        b=bytes.fromhex(r['wire_hex'])
        if len(b)>=60 and b[:4]==b'RNVP' and b[5]==1:
            count=int.from_bytes(b[52:56],'big')
            feedback.append((int(r['received_us']),int.from_bytes(b[44:48],'big'),{int.from_bytes(b[i:i+2],'big') for i in range(60,60+2*count,2)}))
    for c in selected.values():
        if c['kind']=='REPAIR':
            missing={int(x) for x in c['missing_chunks'].split(';') if x}
            require(any(t<=int(c['event_us']) and f==int(c['frame_id']) and missing<=xs for t,f,xs in feedback),'repair uses unreceived missing information')
    requests=[e for e in events if e['event']=='idr_request'];outputs=[e for e in events if e['event']=='idr_output']
    for a,b in zip(requests,requests[1:]):require(int(b['event_us'])-int(a['event_us'])>=500000,'duplicate refresh request')
    pending=None
    for e in events:
        if e['event']=='idr_request':require(pending is None,'overlapping IDR requests');pending=e
        if e['event']=='idr_output':
            require(pending is not None and e['related_frame']==pending['frame_id'] and int(e['frame_id'])>=int(pending['frame_id']),'wrong requested IDR identity')
            require(encoded[int(e['frame_id'])]['idr']=='1','predicted IDR treated as actual');pending=None
    def key(w):return (int(w['frame_id']),int(w['stream_id']),int(w['chunk_index']),w['kind'],int(w['ip_bytes']))
    actual=Counter();parity=0;data={}
    for r in budget:
        b=bytes.fromhex(r['wire_hex'])
        if r['direction']!='uplink' or b[:4]!=b'RNVP' or b[5] not in (0,6):continue
        fid=int.from_bytes(b[16:20],'big');idx=int.from_bytes(b[20:22],'big');stream=int.from_bytes(b[12:16],'big')
        if b[5]==0 and not int.from_bytes(b[36:40],'big')&16:data[(fid,idx)]=b[44:]
        if r['status']!='sent':continue
        actual[(fid,stream,idx,r['kind'],int(r['ip_bytes']))]+=1
        require(0<=origin+int(r['event_us'])-int(encoded[fid]['capture_us'])<200000,'expired packet actually sent')
    for r in wire:
        require(r['status']!='send_error','wire send error')
        if r['status']!='sent':continue
        require(int(r['result'])==int(r['ip_bytes'])-28,'wire length mismatch')
        require(r['generation']==r['active_generation'] and 0<=int(r['event_us'])-int(r['capture_us'])<200000,'stale generation/age sent')
        c=selected[int(r['authorization_id'])]
        require(c['frame_id']==r['frame_id'] and c['generation']==r['generation'] and c['capture_us']==r['capture_us'] and int(c['event_us'])<=int(r['event_us']),'future or unrelated authorization')
        allowed=('REPAIR',) if r['kind']=='retransmission' else ('PROTECT',) if r['kind']=='fec' else ('FRESH','PROTECT')
        require(c['kind'] in allowed,'wire action mismatch')
        latest=max((e for e in events if e['event'] in ('offer','defer') and int(e['event_us'])<=int(r['event_us'])),key=lambda e:int(e['event_us']))
        require(r['active_generation']==latest['generation'],'wire did not use current sender generation')
        if r['kind']=='retransmission':
            # Authorization is the most recent request at send; accepted prior missing chunks remain valid within that AU.
            require(any(p['kind']=='REPAIR' and p['frame_id']==r['frame_id'] and int(p['event_us'])<=int(r['event_us']) and int(r['chunk_index']) in [int(x) for x in p['missing_chunks'].split(';') if x] for p in selected.values()),'unrequested repair packet')
    require(actual==Counter(key(w) for w in wire if w['status']=='sent'),'action/budget wire accounting mismatch')
    for r in budget:
        b=bytes.fromhex(r['wire_hex'])
        if r['direction']!='uplink' or b[:4]!=b'RNVP' or b[5]!=6 or r['status']!='sent':continue
        fid=int.from_bytes(b[16:20],'big');start=int.from_bytes(b[20:22],'big');count=int.from_bytes(b[50:52],'big');length=int.from_bytes(b[48:50],'big')
        require(count>=2 and len(b)==52+length,'invalid XOR header')
        require(all((fid,i) in data for i in range(start,start+count)),'FEC without materialized data')
        expected=bytearray(length)
        for i in range(start,start+count):
            for j,value in enumerate(data[(fid,i)]):expected[j]^=value
        require(bytes(expected)==b[52:],'FEC is not the XOR of actual chunks');parity+=1
    return dict(passed=True,decisions=len(groups),candidate_rows=len(candidates),selected=dict(Counter(c['kind'] for c in selected.values())),
                candidate_reasons=dict(reasons),wire_status=dict(Counter(w['status'] for w in wire)),
                sent_classes=dict(Counter(w['kind'] for w in wire if w['status']=='sent')),xor_packets_checked=parity,
                idr_requests=len(requests),idr_outputs=len(outputs),pending_at_end=pending is not None)


def fault_checks(session):
    base=rows(session/'action_wire.csv');cs=rows(session/'action_candidates.csv');ev=rows(session/'action_events.csv')
    variants=[]
    for field,value in (('active_generation','999999'),('authorization_id','0'),('capture_us','1'),('ip_bytes','1')):
        changed=copy.deepcopy(base);r=next(r for r in changed if r['status']=='sent');r[field]=value;variants.append(('wire_'+field,'action_wire.csv',changed))
    changed=copy.deepcopy(cs);next(c for c in changed if c['selected']=='1')['eligible']='0';variants.append(('illegal_selection','action_candidates.csv',changed))
    changed=copy.deepcopy(ev);next(e for e in changed if e['event']=='idr_output')['related_frame']='0';variants.append(('wrong_idr','action_events.csv',changed))
    original=rows
    for name,file,content in variants:
        with patch(__name__+'.rows',side_effect=lambda p,file=file,content=content:content if p.name==file else original(p)):
            try:action_audit(session)
            except (AssertionError,RuntimeError,ValueError,KeyError):continue
        raise RuntimeError('audit accepted tampered '+name)
    return [v[0] for v in variants]


def main():
    p=argparse.ArgumentParser();p.add_argument('--name',required=True);p.add_argument('--build-name',default='reach_g4_actions_20260924');p.add_argument('--audit-session',type=Path);a=p.parse_args()
    if a.audit_session:
        print(json.dumps(action_audit(a.audit_session)));return 0
    for x in (a.name,a.build_name):require(x.replace('_','').isalnum(),'unsafe name')
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/a.build_name;out=root/'verification'/a.name;out.mkdir(parents=True,exist_ok=False)
    base=read(repo/'config/reach_rt_g2_budget.json');cases=[]
    for name in ('normal','loss','budget','queue','pacer_backlog','downlink_blackout','p_missing'):
        link=dict(uplink=direction(),downlink=direction());budget=copy.deepcopy(base)
        if name=='loss':
            link['uplink']['impairment']=[dict(at_us=t,delay_us=10000,drop=t%200000<12000) for t in range(0,8000000,6000)]
        if name=='budget':budget['uplink']['max_ip_bytes']=45000
        if name=='queue':link['uplink']['capacity']=[dict(at_us=0,bps=200000)];link['uplink']['queue_ip_bytes']=6000
        if name=='pacer_backlog':budget['total']['rate_bps']=100000;budget['uplink']['rate_bps']=100000
        if name=='downlink_blackout':link['downlink']['impairment']=[dict(at_us=0,delay_us=10000,drop=False),dict(at_us=2500000,delay_us=0,drop=True)]
        cases.append(dict(name=name,link=link,budget=budget))
    save(out/'plan.json',dict(cases=cases,duration_s=8,policy='G4-01 wiring only; not R',retries=0))
    report=dict(passed=False,cases=[])
    try:
        for case in cases:
            name=case['name'];save(out/(name+'_link.json'),case['link']);save(out/(name+'_budget.json'),case['budget'])
            cmd=[sys.executable,str(repo/'tools/run_reach_g1.py'),'--name',a.name+'_'+name,'--build-name',a.build_name,'--stage','command_udp','--task','T2','--duration','8','--baseline','G4-01','--state-feedback','--packet-trace','--process-priority','above_normal','--precise-wait','--budget-config',str(out/(name+'_budget.json')),'--link-config',str(out/(name+'_link.json'))]
            if name=='p_missing':cmd+=['--diagnostic-drop-frame','30']
            run=subprocess.run(cmd,cwd=repo,capture_output=True);(out/(name+'.stdout')).write_bytes(run.stdout);(out/(name+'.stderr')).write_bytes(run.stderr)
            session=next((root/'runs'/(a.name+'_'+name)/'sessions').iterdir());entry=dict(name=name,session=str(session),passed=False);report['cases'].append(entry)
            require(run.returncode==0,'native trial failed: '+name)
            entry.update(action=action_audit(session),budget=budget_audit(session),decode=decode_audit(session),state=state_audit(session),integration=integration_audit(session,out/name))
            require(not entry['integration']['allowance_violations'],'accepted pose allowance exceeded')
            act=entry['action']
            if name=='normal':require(act['xor_packets_checked']>0 and act['idr_outputs']>0,'FEC/IDR not exercised');entry['fault_checks']=fault_checks(session)
            if name=='loss':require(act['sent_classes'].get('retransmission',0)>0,'real repair not exercised')
            if name=='budget':require(act['selected'].get('DEFER',0)>0 and act['candidate_reasons'].get('byte_budget_insufficient',0)>0,'budget defer not exercised')
            if name=='pacer_backlog':require(sum(v for k,v in act['wire_status'].items() if k not in ('sent','budget_rejected'))>0,'send-time stale guard not exercised')
            if name=='p_missing':require(next(r for r in rows(session/'encoded.csv') if r['frame_id']=='30')['idr']=='0','diagnostic frame was not P')
            if name=='downlink_blackout':
                world=rows(session/'world.csv');require(max(float(w['v_m_s']) for w in world)>.05,'never moved before blackout')
                require(all(float(w['v_m_s'])==0 for w in world if float(w['simulation_s'])>4),'did not brake after command silence')
            entry['passed']=True;save(out/'report.json',report);print(json.dumps(dict(case=name,action=act)),flush=True)
        report['passed']=True
    except Exception as e:report['error']=repr(e);print(repr(e),flush=True)
    save(out/'report.json',report);return 0 if report['passed'] else 1
if __name__=='__main__':sys.exit(main())
