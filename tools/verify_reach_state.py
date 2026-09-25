"""Independent RSTA wire, receiver provenance and received-prefix causal audit."""
import argparse
from collections import Counter
import copy
import hashlib
import html
import json
from pathlib import Path
import struct
import subprocess
import sys
import zlib
from verify_reach_closed_loop import read, rows, save
from verify_reach_command import require
from verify_reach_budget import budget_audit
from verify_reach_decode import decode_audit


def packet(wire):
    b=bytes.fromhex(wire)
    if len(b)!=208:return None,'invalid_length'
    if b[:4]!=b'RSTA':return None,'invalid_magic'
    if b[4:6]!=b'\0\1':return None,'unknown_version'
    if int.from_bytes(b[6:8],'big')!=208:return None,'invalid_length'
    if int.from_bytes(b[204:],'big')!=zlib.crc32(b[:204]):return None,'invalid_crc'
    fields=[('sequence',24,8),('generated_us',32,8),('task_deadline_us',40,8),('stream',48,4),('complete_frame',52,4),('decoded_frame',56,4),('control_frame',60,4),
            ('generation',64,8),('reference',72,2),('task_stage',74,2),('flags',76,4),('decoded_capture_us',80,8),('decode_wait_us',88,8),('observation_frame',96,4),
            ('missing_frame',100,4),('observation_capture_us',104,8),('observation_received_us',112,8),('state_event_us',120,8),('position_error_micro',140,4),('yaw_error_micro',144,4),
            ('missing_chunks',148,4),('missing_snapshot_us',152,8),('recovery_deadline_us',160,8),('control_generated_us',168,8),('control_sequence',176,8),('command_source_frame',184,4),
            ('capabilities',188,4),('remaining_task_us',192,8),('receiver_event_sequence',200,4)]
    p={k:int.from_bytes(b[o:o+n],'big') for k,o,n in fields};p['session']=b[8:24].hex()
    p.update(dict(zip(('x_micro','y_micro','yaw_micro'),struct.unpack('>iii',b[128:140]))))
    require(p['capabilities']==7 and p['reference']<=3 and p['task_stage']<=3 and p['flags']<=7,'invalid valid-packet schema')
    require(p['remaining_task_us']==max(0,p['task_deadline_us']-p['generated_us']),'wire remaining time')
    return p,'decoded'


def state_audit(session):
    model=read(session/'state_model.json');summary=read(session/'state_summary.json');origin=model['origin_us'];deadline=origin+model['duration_us']
    session_id=read(session/'manifest.json')['session_id'].replace('-','').lower()
    expected_stream=int(session_id[:8],16) or 1
    tx=rows(session/'state_tx.csv');rx=rows(session/'state_rx.csv');estimates=rows(session/'sender_estimates.csv');events=rows(session/'decode_events.csv')
    observations={int(r['frame_id']):r for r in rows(session/'observations.csv')};commands={int(r['sequence']):r for r in rows(session/'commands.csv')}
    chunks=rows(session/'reassembly_events.csv');budget=rows(session/'ip_budget.csv');link=rows(session/'downlink_link.csv');link_summary=read(session/'downlink_link_summary.json')
    require(read(session/'config.effective.json')['state_feedback'] is True,'state feedback not enabled')
    state_budget=[r for r in budget if r['kind']=='state_notification']
    require(Counter((r['wire_hex'],r['status']=='sent') for r in tx)==Counter((r['wire_hex'],r['status']=='sent') for r in state_budget),'notification bypassed common budget')
    arrivals={r['packet_id']:r for r in link if r['event'] in ('admitted','tail_drop')}
    delivered=[(r,arrivals[r['packet_id']]) for r in link if r['event']=='delivered' and arrivals[r['packet_id']]['wire_hex'].startswith('52535441')]
    require(Counter(b['wire_hex'] for a,b in delivered)==Counter(r['wire_hex'] for r in rx),'state receiver bypassed modeled downlink or UDP loss')
    delivered_times={}
    for a,b in delivered:delivered_times.setdefault(b['wire_hex'],[]).append(origin+int(a['actual_send_us']))
    for r in rx:
        times=delivered_times[r['wire_hex']];t=times.pop(0);require(int(r['received_us'])>=t,'notification consumed before delivery')
    require(len(rx)==summary['received']==link_summary['state_delivered_packets'],'state delivery counts')
    prefix_generation=[];generation=0
    for e in events:
        if e['event']=='input_accepted' and e['idr']=='1':generation=int(e['generation'])
        prefix_generation.append(generation)
    checked=set();first_generated=[]
    for row in tx:
        p,why=packet(row['wire_hex'])
        if not p or p['session']!=session_id or p['sequence']>summary['reports'] or p['sequence'] in checked:continue
        checked.add(p['sequence']);first_generated.append(p['generated_us'])
        require(p['generated_us']<=int(row['sent_us']) and p['task_deadline_us']==deadline,'notification time contract')
        q=p['receiver_event_sequence'];require(0<q<=len(events),'receiver prefix missing')
        prefix=events[q-1];require(int(prefix['event_us'])<=p['generated_us'],'future receiver state in notification')
        require(p['stream']==expected_stream,'notification stream differs from session stream')
        transitions=[e for e in events[:q] if e['event'] in ('reference_uncertain','recovery_pending','input_accepted','decoded_output')]
        require(p['state_event_us']==(int(transitions[-1]['event_us']) if transitions else 0),'reference state time differs from committed receiver event')
        for field,log in [('complete_frame','last_complete_frame'),('decoded_frame','last_decoded_frame'),('control_frame','last_control_frame')]:require(p[field]==int(prefix[log]),'receiver snapshot borrowed an unavailable '+field)
        require(p['reference']=={'AwaitingRandomAccess':0,'Synchronized':1,'ReferenceUncertain':2,'RecoveryPending':3}[prefix['state']] and p['generation']==prefix_generation[q-1],'receiver generation/state wrong')
        if p['decoded_frame']:
            e=next(e for e in events[:q] if e['event']=='decoded_output' and int(e['frame_id'])==p['decoded_frame'])
            require(p['decoded_capture_us']==int(e['capture_us']) and p['decode_wait_us']==int(e['decoder_wait_us']),'decoder timing not from actual output')
        if p['observation_frame']:
            o=observations[p['observation_frame']]
            require(p['observation_capture_us']==int(o['capture_us']) and p['observation_received_us']==int(o['received_us'])<=p['generated_us'],'observation provenance')
            require(any(e['event'].startswith('recognition_') and int(e['frame_id'])==p['observation_frame'] for e in events[:q]),'observation not yet published')
            expected_valid=o['valid']=='1' and p['generated_us']-p['observation_capture_us']<=200000
            require(bool(p['flags']&1)==expected_valid,'observation loss flag wrong')
            if o['valid']=='1':
                for key,source in [('x_micro','x_m'),('y_micro','y_m'),('yaw_micro','yaw_rad')]:require(abs(p[key]-float(o[source])*1000000)<=.501,'pose is not received-image estimate')
                for key,source in [('position_error_micro','position_error_m'),('yaw_error_micro','yaw_error_rad')]:require(-.01<=p[key]-float(o[source])*1000000<=1.01,'wire error allowance not conservatively rounded')
        if p['control_sequence']:
            c=commands[p['control_sequence']];require(p['control_generated_us']==int(c['generated_us'])<=p['generated_us'] and p['command_source_frame']==int(c['source_frame_id']),'task command provenance')
            require(p['task_stage']=={'OBSERVE':0,'APPROACH':1,'ALIGN':2,'HOLD':3}[c['state']],'task phase not image controller phase')
            require(bool(p['flags']&2)==bool(int(c['estimated_complete']) and p['flags']&1),'task completion flag confused with evaluator truth')
        if p['flags']&4:
            require(any(int(a['frame_id'])==p['missing_frame'] and int(a['event_us'])==p['missing_snapshot_us'] and int(a['missing_chunks'])==p['missing_chunks'] and int(a['recovery_deadline_us'])==p['recovery_deadline_us'] for a in chunks),'missing chunks snapshot fabricated')
    require(len(checked)==summary['reports'],'some generated reports were not audited')
    require(all(b-a>=20000 for a,b in zip(first_generated,first_generated[1:])),'notification interval unbounded')
    accepted=None;accepted_us=highest=highest_generated=0;statuses=Counter();index=0;causal=0
    def receive(row):
        nonlocal accepted,accepted_us,highest,highest_generated
        now=int(row['received_us']);p,status=packet(row['wire_hex'])
        if p:
            if p['session']!=session_id:status='wrong_session'
            elif p['generated_us']>now:status='future_notification'
            elif p['generated_us']<origin or p['task_deadline_us']!=deadline:status='session_contract_mismatch'
            elif p['sequence']==highest:status='duplicate'
            elif p['sequence']<highest:status='reordered'
            elif p['generated_us']<highest_generated:status='generation_reversed'
            else:
                highest=p['sequence'];highest_generated=p['generated_us'];status='expired_on_arrival' if now-p['generated_us']>=250000 else 'accepted'
                if status=='accepted':accepted=p;accepted_us=now
        require(row['status']==status,'state receive status wrong')
        require(int(row['highest_sequence'])==highest and int(row['accepted_sequence'])==(accepted['sequence'] if accepted else 0),'state acceptance ordering wrong');statuses[status]+=1
    for e in estimates:
        now=int(e['event_us'])
        while index<len(rx) and int(rx[index]['received_us'])<=now:receive(rx[index]);index+=1
        n=lambda key:int(e[key]);require(n('accepted_sequence')==(accepted['sequence'] if accepted else 0),'sender used unreceived or rejected report');require(n('received_us')==accepted_us,'sender receipt timestamp invented')
        require(n('remaining_task_us')==max(0,deadline-now),'sender remaining time')
        if not accepted:
            require(n('generation')==n('decoded_frame')==n('control_frame')==n('observation_usable')==n('notification_live')==0 and e['decision']=='awaiting_notification','hidden state visible before any arrival')
        else:
            p=accepted;age=now-p['generated_us'];obsage=now-p['observation_capture_us'] if p['observation_capture_us'] else 0;live=age<250000;sync=live and p['reference']==1;usable=live and bool(p['flags']&1) and obsage<=200000
            require(n('notification_age_us')==age and n('observation_age_us')==obsage,'sender age rebased on receipt')
            for field in ('generation','complete_frame','decoded_frame','control_frame','observation_frame','x_micro','y_micro','yaw_micro'):require(n(field)==p[field],'sender estimate used hidden receiver field '+field)
            require(n('generated_us')==p['generated_us'] and n('reported_reference')==p['reference'],'sender borrowed newer report')
            require(n('notification_live')==int(live) and n('reference_reported_synchronized')==int(sync) and n('observation_usable')==int(usable),'sender stale-state eligibility wrong')
            pr=p['position_error_micro']+(3*obsage+9)//10 if p['observation_capture_us'] else 0;yr=p['yaw_error_micro']+(4*obsage+4)//5 if p['observation_capture_us'] else 0
            require(n('position_radius_micro')==pr and n('yaw_radius_micro')==yr,'uncertainty does not age from observed capture')
            decision='task_deadline' if now>=deadline else 'notification_stale' if not live else 'reference_unconfirmed' if not sync else 'observation_unavailable' if not usable else 'reported_state_usable'
            require(e['decision']==decision,'sender decision not causal')
        causal+=1
    while index<len(rx):receive(rx[index]);index+=1
    require(dict(statuses)==summary['statuses'] and causal==summary['estimates'],'state summary mismatch')
    require(sum(r['status']=='sent' for r in tx)==summary['sent'] and sum(r['status']=='budget_rejected' for r in tx)==summary['budget_rejected'],'notification tx counters')
    return dict(passed=True,summary=summary,receiver_reports_audited=len(checked),causal_estimates_audited=causal,
                notification_ip_bytes=sum(int(r['attempted_ip_bytes']) for r in state_budget),
                stale_estimates=sum(e['decision']=='notification_stale' for e in estimates),unknown_estimates=sum(e['decision']=='awaiting_notification' for e in estimates),
                usable_estimates=sum(e['decision']=='reported_state_usable' for e in estimates))


def render(out,report):
    body=''
    for c in report['cases']:
        s=c.get('state',{});t=s.get('summary',{});body+=f"<tr><td>{html.escape(c['name'])}</td><td>{c['passed']}</td><td>{t.get('sent','—')}</td><td>{t.get('received','—')}</td><td>{s.get('causal_estimates_audited','—')}</td><td>{s.get('usable_estimates','—')}</td><td>{s.get('stale_estimates','—')}</td><td>{s.get('notification_ip_bytes','—')}</td></tr>"
    (out/'index.html').write_text('<!doctype html><html lang="ja"><meta charset="utf-8"><title>Reach-RT G2-05</title><style>body{font:16px system-ui;background:#edf3fa;color:#19334c;margin:40px}table{background:white;border-collapse:collapse;width:100%}td,th{text-align:left;padding:14px;border-bottom:1px solid #d8e2ee}h1{color:#134e73}</style><h1>Reach-RT / G2-05 到着した通知だけから推定</h1><p>受信画像・復号状態 → 208 byte通知 → 共通IP予算・下り回線 → 送信側の推定</p><p>各推定を、その時刻までに受信・採用した電文から独立に再計算。通知が届かない間は誤差余裕を拡大し、有効性を失効させます。</p><p>検証判定: '+('PASS' if report['passed'] else 'FAIL')+'。研究方式の性能優位性や作業成功率の検証ではありません。</p><table><tr><th>条件</th><th>合格</th><th>通知送信</th><th>通知受信</th><th>推定監査</th><th>利用可能判定</th><th>通知失効</th><th>通知IP byte</th></tr>'+body+'</table></html>',encoding='utf-8')


def main():
    p=argparse.ArgumentParser();p.add_argument('--name',required=True);p.add_argument('--build-name',default='reach_g2_feedback_20260922');args=p.parse_args()
    for value in (args.name,args.build_name):require(value.replace('_','').replace('-','').isalnum(),'invalid name')
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/args.build_name;out=root/'verification'/args.name;out.mkdir(parents=True,exist_ok=False)
    def point(at,delay=0,drop=False):return dict(at_us=at,delay_us=delay,drop=drop)
    def direction(points):return dict(queue_ip_bytes=65536,capacity=[dict(at_us=0,bps=6000000)],impairment=points)
    cases=[('normal',[point(0,20000)],'normal',None),('blackout',[point(0,20000),point(1200000,drop=True),point(2500000,20000)],'normal',None),
           ('overtaken',[point(0,180000),point(1500000)],'normal',None),('expired',[point(0,300000)],'normal',None),
           ('duplicate',[point(0,20000)],'duplicate',None),('invalid',[point(0,20000)],'invalid',None),('budget_cut',[point(0,20000)],'normal',15000),
           ('no_notification',[point(0,drop=True)],'normal',None)]
    report=dict(passed=False,executable_sha256=hashlib.sha256((root/'bin/Release/GE3.exe').read_bytes()).hexdigest(),cases=[])
    try:
        for name,points,diagnostic,cap in cases:
            link=dict(uplink=direction([point(0)]),downlink=direction(points));lp=out/(name+'_link.json');save(lp,link)
            budget=read(repo/'config/reach_rt_g2_budget.json')
            if cap:budget['downlink']['max_ip_bytes']=cap
            bp=out/(name+'_budget.json');save(bp,budget);run=args.name+'_'+name
            result=subprocess.run([sys.executable,str(repo/'tools/run_reach_g1.py'),'--stage','command_udp','--task','T2','--duration','4','--name',run,'--build-name',args.build_name,'--link-config',str(lp),'--budget-config',str(bp),'--state-feedback','--state-diagnostic',diagnostic,'--packet-trace'],cwd=repo,capture_output=True)
            (out/(name+'.stdout.txt')).write_bytes(result.stdout);(out/(name+'.stderr.txt')).write_bytes(result.stderr)
            session=next((root/'runs'/run/'sessions').iterdir());c=dict(name=name,session=str(session),passed=False);report['cases'].append(c)
            require(result.returncode==0,'native run failed: '+str(read(session/'summary.json')))
            require(read(session.parent.parent/'launch.json')['executable_sha256']==report['executable_sha256'],'binary changed')
            require(read(session/'summary.json')['clock_tick_counts']==dict(physics=400,camera=120,control=80),'clock count mismatch')
            c['state']=state_audit(session);c['budget']=budget_audit(session);c['decode']=decode_audit(session)
            s=c['state'];statuses=s['summary']['statuses'];est=rows(session/'sender_estimates.csv')
            if name=='normal':require(statuses.get('accepted',0)>60 and s['usable_estimates']>20,'normal notification pipeline unavailable')
            if name in ('blackout','budget_cut'):
                require(s['stale_estimates']>10 and s['usable_estimates']>0,'notification loss did not expire sender state')
                stale=[e for e in est if e['decision']=='notification_stale'];require(any(int(b['position_radius_micro'])>int(a['position_radius_micro']) and a['accepted_sequence']==b['accepted_sequence'] for a,b in zip(stale,stale[1:])),'silence did not grow uncertainty')
            if name=='blackout':
                require(any(int(e['event_us'])>read(session/'state_model.json')['origin_us']+2800000 and e['notification_live']=='1' for e in est),'no recovery after blackout')
                missing=[r for r in rows(session/'state_tx.csv') if r['status']=='sent' and 1300000<int(r['sent_us'])-read(session/'state_model.json')['origin_us']<2300000]
                require(len(missing)>10 and len({packet(r['wire_hex'])[0]['decoded_frame'] for r in missing})>5,'receiver state did not advance during notification blackout')
            if name=='overtaken':require(statuses.get('reordered',0)>0,'old notification overwriting case absent')
            if name=='expired':require(statuses.get('expired_on_arrival',0)>50 and s['unknown_estimates']==80,'expired notifications became current information')
            if name=='duplicate':require(statuses.get('duplicate',0)>50,'duplicate diagnostic absent')
            if name=='invalid':require(all(statuses.get(k,0)==1 for k in ('wrong_session','invalid_crc','invalid_length','unknown_version','future_notification')),'invalid notification guards not exercised')
            if name=='budget_cut':require(s['summary']['budget_rejected']>10,'state notifications bypassed budget cutoff')
            if name=='no_notification':require(s['summary']['received']==0 and s['unknown_estimates']==80 and c['decode']['decoded']>20,'hidden receiver state leaked without delivery')
            c['passed']=True;save(out/'report.json',report);print(json.dumps(dict(case=name,**s)),flush=True)
        report['passed']=True
    except Exception as error:report['error']=str(error);print(str(error),file=sys.stderr)
    save(out/'report.json',report);render(out,report);return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
