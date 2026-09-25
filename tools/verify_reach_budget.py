"""Audit wire-derived IPv4 bytes and every shared/directional budget decision."""
import argparse
from collections import Counter,defaultdict
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from verify_reach_command import audit as command_audit,require
from verify_reach_link import link_audit
from verify_reach_closed_loop import read,rows,save


def packet_class(wire):
    b=bytes.fromhex(wire)
    if b[:4]==b'RCMD' and len(b)==88:return 'command'
    if b[:4]==b'RSTA':return 'state_notification'
    require(len(b)>=44 and b[:4]==b'RNVP' and b[4]==1 and int.from_bytes(b[6:8],'big')==44,'unknown wire protocol')
    typ=b[5];flags=int.from_bytes(b[36:40],'big')
    if flags&16:return 'retransmission'
    if typ==0:return 'video_idr' if flags&1 else 'video_delta'
    if typ==1:return 'nack' if int.from_bytes(b[52:56],'big') else 'ack'
    return {2:'ping',3:'pong',4:'control',5:'transport_feedback',6:'fec'}[typ]


def budget_audit(session):
    config=read(session/'config.effective.json');limits=config['ip_budget'];summary=read(session/'ip_budget_summary.json')
    entries=rows(session/'ip_budget.csv');credit={d:limits[d]['burst_ip_bytes']*8000000 for d in ('total','uplink','downlink')};used=Counter();previous=0
    totals=defaultdict(Counter);statuses=Counter();sent=defaultdict(Counter)
    for i,r in enumerate(entries,1):
        now=int(r['event_us']);direction=r['direction'];kind=packet_class(r['wire_hex']);b=len(bytes.fromhex(r['wire_hex']))+28
        require(int(r['event_id'])==i and now>=previous,'budget ledger order')
        require(direction in ('uplink','downlink') and r['kind']==kind,'incorrect direction/class label')
        require(int(r['ip_bytes'])==b and int(r['payload_bytes'])==b-28,'incorrect IPv4 + UDP byte count')
        for d in credit:credit[d]=min(limits[d]['burst_ip_bytes']*8000000,credit[d]+limits[d]['rate_bps']*(now-previous))
        previous=now;reason='sent'
        if now>=summary['duration_us']:reason='trial_closed'
        else:
            for d in ('total',direction):
                if used[d]+b>limits[d]['max_ip_bytes']:reason='total_byte_limit' if d=='total' else 'direction_byte_limit';break
                if credit[d]<b*8000000:reason='total_rate_limit' if d=='total' else 'direction_rate_limit';break
        require(r['status']==reason,'wrong budget admission/rejection')
        c=totals[direction+'/'+kind];c['offered_ip_bytes']+=b;statuses[reason]+=1
        if reason=='sent':
            for d in ('total',direction):used[d]+=b;credit[d]-=b*8000000
            c['attempted_ip_bytes']+=b;c['sent_ip_bytes']+=b;sent[direction][r['wire_hex']]+=1
            require(int(r['attempted_ip_bytes'])==b and int(r['discarded_before_send_bytes'])==0,'sent/rejected double count')
        else:
            c['discarded_before_send_bytes']+=b
            require(int(r['attempted_ip_bytes'])==0 and int(r['discarded_before_send_bytes'])==b,'pre-send discard charged')
        require(int(r['total_used_ip_bytes'])==used['total'] and int(r['direction_used_ip_bytes'])==used[direction],'cumulative ledger wrong')
    require(not summary['error'],'transport error hidden')
    require(used['total']==used['uplink']+used['downlink']==summary['attempted_ip_bytes'],'direction total mismatch')
    require(summary['uplink_ip_bytes']==used['uplink'] and summary['downlink_ip_bytes']==used['downlink'],'direction summary')
    require(set(totals)==set(summary['classes']),'missing/extra class')
    for k,c in summary['classes'].items():require(all(totals[k][name]==v for name,v in c.items()),'class summary mismatch '+k)
    links={}
    for d in ('uplink','downlink'):
        links[d]=link_audit(session,d);data=rows(session/(d+'_link.csv'))
        require(links[d]['origin_us']==summary['origin_us'],'budget/link clock mismatch')
        arrivals={r['packet_id']:r for r in data if r['event'] in ('admitted','tail_drop')}
        require(sent[d]==Counter(r['wire_hex'] for r in arrivals.values()),'budget gate bypass or UDP loss '+d)
        require(links[d]['offered_ip_bytes']==used[d],'link accounting differs from sender IP attempts')
        if d=='downlink':
            feedback=[arrivals[r['packet_id']]['wire_hex'] for r in data if r['event']=='delivered' and packet_class(arrivals[r['packet_id']]['wire_hex']) not in ('command','state_notification')]
            actual=rows(session/'feedback_rx.csv')
            require(Counter(feedback)==Counter(r['wire_hex'] for r in actual),'feedback bypassed downlink or delivery lost')
            require(len(feedback)==summary['feedback_received']==links[d]['feedback_delivered_packets'],'feedback counts')
            require(all(packet_class(r['wire_hex'])==r['kind'] for r in actual),'feedback classification')
            if config.get('state_feedback'):
                states=[arrivals[r['packet_id']]['wire_hex'] for r in data if r['event']=='delivered' and packet_class(arrivals[r['packet_id']]['wire_hex'])=='state_notification']
                require(Counter(states)==Counter(r['wire_hex'] for r in rows(session/'state_rx.csv')),'state notifications bypassed downlink or delivery lost')
                require(len(states)==links[d]['state_delivered_packets'],'state relay counts')
        expected=(links[d]['offered_ip_bytes']-links[d]['tail_dropped_ip_bytes'])*8000000-sum(int(r['remaining_work']) for r in data if r['event']=='cancelled_at_close')
        require(expected==links[d]['serviced_work_bit_microseconds'],'serviced work accounting')
    commands=command_audit(session,link_loss=True)
    tx=rows(session/'command_tx.csv');require(sum(r['event']=='budget_rejected' for r in tx)==commands['summary']['budget_rejected'],'command pre-send discards')
    return dict(passed=True,summary=summary,status_counts=dict(statuses),uplink=links['uplink'],downlink=links['downlink'],command=commands,ledger_events=len(entries),discarded_before_send_bytes=sum(c['discarded_before_send_bytes'] for c in summary['classes'].values()))


def main():
    p=argparse.ArgumentParser();p.add_argument('--name',required=True);p.add_argument('--build-name',default='reach_g2_budget_20260922');args=p.parse_args()
    for v in (args.name,args.build_name):
        if not v.replace('_','').replace('-','').isalnum():p.error('invalid name')
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/args.build_name;out=root/'verification'/args.name;out.mkdir(parents=True,exist_ok=False)
    base=read(repo/'config/reach_rt_g2_budget.json')
    def link(rate,queue=65536,loss=False):return dict(queue_ip_bytes=queue,capacity=[dict(at_us=0,bps=rate)],impairment=[dict(at_us=0,drop=False,delay_us=0)]+([dict(at_us=1500000,drop=True,delay_us=0),dict(at_us=1600000,drop=False,delay_us=0)] if loss else []))
    normal=dict(uplink=link(3000000),downlink=link(500000,16384));cases=[]
    cases.append(('all_classes',6,copy.deepcopy(base),normal))
    aggregate=copy.deepcopy(base);aggregate['total'].update(rate_bps=0,burst_ip_bytes=10000,max_ip_bytes=10000)
    cases.append(('aggregate_cap',4,aggregate,normal))
    rate=copy.deepcopy(base);rate['total'].update(rate_bps=128000,burst_ip_bytes=4096)
    cases.append(('aggregate_rate',4,rate,normal))
    down=copy.deepcopy(base);down['fec_group_chunks']=0;down['downlink'].update(rate_bps=500000,burst_ip_bytes=16384,max_ip_bytes=26000)
    cases.append(('downlink_cap_stop',6,down,dict(uplink=link(6000000),downlink=link(500000,16384))))
    lost=copy.deepcopy(normal);lost['uplink']=link(3000000,65536,True);lost['downlink']=link(500000,16384,True)
    cases.append(('loss_charged',4,copy.deepcopy(base),lost))
    report=dict(passed=False,executable_sha256=hashlib.sha256((root/'bin/Release/GE3.exe').read_bytes()).hexdigest(),cases=[])
    try:
        for name,duration,budget,link_config in cases:
            bp=out/(name+'_budget.json');lp=out/(name+'_link.json');save(bp,budget);save(lp,link_config)
            run=args.name+'_'+name
            r=subprocess.run([sys.executable,str(repo/'tools/run_reach_g1.py'),'--stage','command_udp','--build-name',args.build_name,'--task','T2','--duration',str(duration),'--name',run,'--link-config',str(lp),'--budget-config',str(bp),'--packet-trace'],cwd=repo,capture_output=True)
            (out/(name+'.stdout.txt')).write_bytes(r.stdout);(out/(name+'.stderr.txt')).write_bytes(r.stderr)
            session=next((root/'runs'/run/'sessions').iterdir());item=dict(name=name,session=str(session),passed=False,duration_s=duration);report['cases'].append(item)
            require(r.returncode==0,'native execution failed: '+str(read(session/'summary.json')))
            require(read(session.parent.parent/'launch.json')['executable_sha256']==report['executable_sha256'],'binary changed')
            require(read(session/'config.effective.json')['ip_budget']==budget,'effective budget differs from declared limits')
            require(read(session/'summary.json')['clock_tick_counts']==dict(physics=duration*100,camera=duration*30,control=duration*20),'clock count')
            result=budget_audit(session);item.update({k:v for k,v in result.items() if k!='passed'});classes=result['summary']['classes'];statuses=result['status_counts']
            if name=='all_classes':
                for kind in ('uplink/video_idr','uplink/video_delta','uplink/fec','uplink/retransmission','downlink/ack','downlink/nack','downlink/transport_feedback','downlink/command'):
                    require(classes.get(kind,{}).get('attempted_ip_bytes',0)>0,'class not exercised '+kind)
            if name=='aggregate_cap':require(statuses.get('total_byte_limit',0)>0 and result['summary']['attempted_ip_bytes']<=10000,'shared maximum not enforced')
            if name=='aggregate_rate':require(statuses.get('total_rate_limit',0)>0,'aggregate token exhaustion absent')
            if name=='downlink_cap_stop':
                require(statuses.get('direction_byte_limit',0)>0,'direction maximum not reached')
                world=rows(session/'world.csv');require(max(float(x['v_m_s']) for x in world)>.25,'robot never reached test speed')
                require(all(float(x['v_m_s'])==0 for x in world if float(x['simulation_s'])>5.7),'robot did not stop after command budget exhaustion')
                require(any(x['reason']=='watchdog_timeout' for x in rows(session/'udp_applied_commands.csv')),'watchdog never tripped')
            if name=='loss_charged':require(result['uplink']['trace_dropped_ip_bytes']>0 and result['downlink']['trace_dropped_ip_bytes']>0,'loss trace absent')
            item['passed']=True;save(out/'report.json',report);print(json.dumps(dict(case=name,passed=True,ip_bytes=result['summary']['attempted_ip_bytes'],pre_send_discard=result['discarded_before_send_bytes'],statuses=statuses)),flush=True)
        report['passed']=True
    except Exception as e:report['error']=str(e);print(str(e),file=sys.stderr)
    save(out/'report.json',report);return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
