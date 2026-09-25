"""G2-04: audit independent receiver stages against actual MF output and control."""
import argparse
from collections import Counter
import hashlib
import html
import json
from pathlib import Path
import subprocess
import sys
from verify_reach_closed_loop import rows, read, save
from verify_reach_command import require, audit as command_audit
from verify_reach_link import link_audit


def decode_audit(session):
    events=rows(session/'decode_events.csv');chunks=rows(session/'reassembly_events.csv')
    encoded={int(r['frame_id']):r for r in rows(session/'encoded.csv')}
    pixels={int(r['frame_id']):r for r in rows(session/'decoded_audit.csv')}
    observations={int(r['frame_id']):r for r in rows(session/'observations.csv')}
    commands={int(r['sequence']):r for r in rows(session/'commands.csv')}
    counts=Counter(r['event'] for r in events)
    complete={};dequeued=set();validated=set();accepted={};decoded={};abandoned=set();used=set()
    generation=0;state='AwaitingRandomAccess';last_complete=last_decoded=last_control=0
    model=read(session/'decode_model.json');require(model['late_replay_supported'] is False,'replay capability changed')
    task_deadlines=set()
    for number,r in enumerate(events,1):
        n=lambda name:int(r[name])
        fid=n('frame_id');event=r['event'];now=n('event_us');capture=n('capture_us');gen=n('generation')
        require(n('sequence')==number,'journal sequence')
        require(n('display_deadline_us')==(capture+200000 if capture else 0),'display deadline')
        require(n('display_timely')==int(bool(capture and capture<=now<=capture+200000)),'display time classification')
        require(n('decoder_wait_us')==(now-n('decoder_input_us') if n('decoder_input_us') else 0),'decoder wait clock')
        require(n('task_deadline_us')>0,'task deadline absent');task_deadlines.add(n('task_deadline_us'))
        if capture:
            require(fid in encoded and capture==int(encoded[fid]['capture_us']),'wrong capture identity')
            require(n('stream_id')==int(encoded[fid]['stream_id']),'stream identity')
        if event=='reassembled':
            require(fid not in complete,'duplicate completion');complete[fid]=r;last_complete=fid
        elif event=='decode_dequeued':
            require(fid in complete,'decode queue has no reassembly');dequeued.add(fid)
        elif event=='au_validated':
            require(fid in dequeued,'validation before dequeue');validated.add(fid)
            require(n('idr')==int(encoded[fid]['idr']),'NAL IDR mismatch')
        elif event=='reference_uncertain':state='ReferenceUncertain'
        elif event=='recovery_pending':state='RecoveryPending'
        elif event=='input_accepted':
            require(fid in validated and fid not in accepted,'input not uniquely validated')
            if n('idr'):generation+=1;state='RecoveryPending'
            require(generation>0 and gen==generation,'generation not based on accepted IDR')
            require(n('idr')==int(encoded[fid]['idr']),'input IDR mismatch')
            require(now>=n('decoder_input_us')>=int(complete[fid]['event_us']),'input timing')
            accepted[fid]=r
        elif event=='input_abandoned':
            require(fid in accepted and fid not in decoded and fid not in abandoned,'invalid abandoned input')
            abandoned.add(fid)
        elif event=='decoded_output':
            require(fid in accepted and fid not in abandoned and fid not in decoded,'output without live input')
            require(gen==int(accepted[fid]['generation']) and n('decoder_input_us')==int(accepted[fid]['decoder_input_us']),'output borrowed current input identity')
            require(fid in pixels and pixels[fid]['identity_match']=='1','real image audit missing')
            require(now<=int(pixels[fid]['decoded_us']),'output clock later than published frame')
            decoded[fid]=r;last_decoded=fid
            if n('trusted') and gen==generation:state='Synchronized'
            elif not n('trusted'):state='ReferenceUncertain'
        elif event.startswith('display_') or event.startswith('recognition_'):
            require(fid in decoded and gen==int(decoded[fid]['generation']),'adoption without real output')
            if event=='display_adopted':require(n('trusted') and n('display_timely'),'unsafe display adoption')
            if event.startswith('recognition_'):
                require(fid in observations and int(observations[fid]['valid'])==int(event=='recognition_accepted'),'recognition outcome mismatch')
        elif event.startswith('control_'):
            command=commands[n('command_sequence')];require(n('command_sequence') not in used,'duplicate command event');used.add(n('command_sequence'))
            require(fid==int(command['source_frame_id']) and now==int(command['generated_us']),'command provenance')
            require(n('command_deadline_us')==int(command['valid_until_us'])==now+100000,'command deadline')
            expected_use=command['reason'] in ('image_feedback','wall_margin_stop','uncertainty_stop')
            require((event=='control_used')==expected_use,'rejected observation counted as control use')
            if event=='control_used':
                require(fid in decoded and observations[fid]['valid']=='1' and n('display_timely'),'control used unavailable/expired image')
                require(gen==int(decoded[fid]['generation']),'control generation mismatch');last_control=fid
        require(r['state']==state,'receiver state transition')
        require((n('last_complete_frame'),n('last_decoded_frame'),n('last_control_frame'))==(last_complete,last_decoded,last_control),'stage snapshots mixed')
    require(len(task_deadlines)==1,'task deadline changed')
    require(set(accepted)==set(decoded)|abandoned,'unaccounted decoder inputs at drain')
    require(set(pixels)==set(decoded),'pixel outputs missing from decode ledger')
    require(used==set(commands),'commands missing from ledger')
    summary=read(session/'robot_video_summary.json')
    require(len(complete)==summary['udp_completed_frames'] and len(dequeued)==summary['decode_pop_count'] and len(decoded)==summary['decoded'],'stage count mismatch')
    chunk_completions=set();partial=deadlines=0
    for r in chunks:
        total=int(r['total_chunks']);received=int(r['received_chunks']);missing=int(r['missing_chunks'])
        indices=[int(v) for v in r['missing_indices'].split(';') if v]
        require(total==received+missing and len(indices)==len(set(indices)) and all(0<=i<total for i in indices),'invalid chunk snapshot')
        require(int(r['indices_complete'])==int(len(indices)==missing),'unknown chunks silently marked known')
        if missing:partial+=1
        if int(r['recovery_deadline_us']):deadlines+=1
        if r['event']=='completed':
            require(missing==0,'incomplete AU called complete');chunk_completions.add(int(r['frame_id']))
    require(chunk_completions==set(complete),'reassembly outcome differs from complete queue')
    require(not counts['decoder_failed'],'decoder failure occurred')
    return dict(passed=True,counts=dict(counts),reassembled=len(complete),accepted=len(accepted),decoded=len(decoded),abandoned=len(abandoned),generations=generation,
                recognized=counts['recognition_accepted'],display_adopted=counts['display_adopted'],control_used=counts['control_used'],
                partial_chunk_snapshots=partial,recovery_deadline_snapshots=deadlines,states=sorted({r['state'] for r in events}))


def render(out,report):
    cards=[]
    for c in report['cases']:
        a=c.get('decode',{});session=Path(c['session'])
        table=''.join('<tr>'+''.join('<td>'+html.escape(str(v))+'</td>' for v in [r['event_us'],r['event'],r['frame_id'],r['generation'],r['state'],r['reason']])+'</tr>'
            for r in rows(session/'decode_events.csv') if r['event'] in ('reference_uncertain','recovery_pending','input_abandoned','encoder_keyframe_requested') or r['event']=='input_accepted' and r['idr']=='1')
        cards.append(f"<section><h2>{html.escape(c['name'])} — {'PASS' if c['passed'] else 'FAIL'}</h2><p>再構成 {a.get('reassembled','—')} → デコーダ受理 {a.get('accepted','—')} → 実画像 {a.get('decoded','—')} → 表示採用 {a.get('display_adopted','—')} ／ 認識採用 {a.get('recognized','—')}</p><p>参照世代 {a.get('generations','—')} ／ 出力前に破棄 {a.get('abandoned','—')} ／ 制御利用（繰り返しを含む） {a.get('control_used','—')}</p><details><summary>IDR・参照状態の経緯</summary><table><tr><th>単調時刻 µs</th><th>イベント</th><th>画像ID</th><th>世代</th><th>状態</th><th>理由</th></tr>{table}</table></details></section>")
    (out/'index.html').write_text('<!doctype html><html lang="ja"><meta charset="utf-8"><title>Reach-RT G2-04</title><style>body{font:16px system-ui;background:#edf3fa;color:#17304a;margin:40px;max-width:1250px}section{background:white;padding:24px;margin:20px 0;border-radius:12px}h1{color:#143e67}table{border-collapse:collapse;width:100%;font-size:13px}td,th{text-align:left;padding:8px;border-bottom:1px solid #dae2ec}summary{cursor:pointer}</style><h1>Reach-RT / G2-04 復号状態の追跡</h1><p>パケットの再構成と、実際に使える画像を別々に検証。実H.264・UDP・単調時計による結果です。</p><p>判定: '+('PASS' if report['passed'] else 'FAIL')+' ／ 作業成功率や研究方式の優位性を示す試験ではありません。</p>'+''.join(cards)+'</html>',encoding='utf-8')


def main():
    p=argparse.ArgumentParser();p.add_argument('--name',required=True);p.add_argument('--build-name',default='reach_g2_decode_20260922');args=p.parse_args()
    for v in (args.name,args.build_name):
        if not v.replace('_','').replace('-','').isalnum():p.error('invalid name')
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/args.build_name;out=root/'verification'/args.name;out.mkdir(parents=True,exist_ok=False)
    def direction(points=None):return dict(queue_ip_bytes=65536,capacity=[dict(at_us=0,bps=6000000)],impairment=points or [dict(at_us=0,drop=False,delay_us=0)])
    def point(at,delay=0,drop=False):return dict(at_us=at,delay_us=delay,drop=drop)
    cases=[('ideal',[],0,'auto',False),('initial_idr_missing',[],1,'auto',False),('p_missing',[],20,'auto',False),
           ('late_display',[],0,'auto',False),('late_transport',[point(0,250000)],0,'auto',False),('overtaken',[point(0,300000),point(1500000)],0,'auto',False),
           ('partial_loss',[point(0),point(1500000,drop=True),point(1600000)],0,'auto',True),('software',[],0,'software',False)]
    report=dict(passed=False,executable_sha256=hashlib.sha256((root/'bin/Release/GE3.exe').read_bytes()).hexdigest(),cases=[])
    try:
        for name,points,drop,encoder,budget in cases:
            link=dict(uplink=direction(points),downlink=direction());lp=out/(name+'_link.json');save(lp,link)
            run=args.name+'_'+name
            opts=['--diagnostic-drop-frame',str(drop)] if drop else []
            if name=='late_display':opts+=['--diagnostic-recognition-delay-ms','300']
            if budget:opts+=['--budget-config',str(repo/'config/reach_rt_g2_budget.json')]
            r=subprocess.run([sys.executable,str(repo/'tools/run_reach_g1.py'),'--stage','command_udp','--build-name',args.build_name,'--task','T2','--duration','4','--name',run,'--link-config',str(lp),'--encoder',encoder,'--packet-trace',*opts],cwd=repo,capture_output=True)
            (out/(name+'.stdout.txt')).write_bytes(r.stdout);(out/(name+'.stderr.txt')).write_bytes(r.stderr)
            session=next((root/'runs'/run/'sessions').iterdir());item=dict(name=name,session=str(session),passed=False);report['cases'].append(item)
            require(r.returncode==0,'native trial failed: '+str(read(session/'summary.json')))
            require(read(session.parent.parent/'launch.json')['executable_sha256']==report['executable_sha256'],'binary changed')
            require(read(session/'summary.json')['clock_tick_counts']==dict(physics=400,camera=120,control=80),'clock tick count')
            result=decode_audit(session);item['decode']=result
            item['links']={d:link_audit(session,d) for d in ('uplink','downlink')};item['command']=command_audit(session,link_loss=True)
            events=rows(session/'decode_events.csv');encoded=rows(session/'encoded.csv')
            if name in ('ideal','software'):require(result['reassembled']==result['accepted']==result['decoded']==120,'ideal stage count')
            if drop:
                row=next(v for v in encoded if int(v['frame_id'])==drop)
                require(int(row['idr'])==int(drop==1) and row['diagnostic_drop']=='1','diagnostic did not drop declared IDR/P')
                from inspect_reach_g0_codec import Bits, rbsp, split_nals
                vcl=[]
                for nal in split_nals((session/'diagnostic_dropped_au.h264').read_bytes()):
                    if nal[0]&31 in (1,5):
                        bits=Bits(rbsp(nal));bits.ue();slice_type=bits.ue()
                        require(slice_type<=9,'invalid diagnostic slice type')
                        vcl.append(dict(nal_type=nal[0]&31,slice_type=slice_type,nal_ref_idc=(nal[0]>>5)&3))
                require(vcl and all(s['nal_type']==5 and s['slice_type']%5==2 if drop==1 else s['nal_type']==1 and s['slice_type']%5==0 and s['nal_ref_idc']>0 for s in vcl),'dropped AU not IDR / reference P picture')
                item['dropped_au_slices']=vcl
                require(result['reassembled']>result['decoded'] and result['counts'].get('input_rejected',0)>0,'missing reference not distinguished')
                require(any(e['event']=='decoded_output' and int(e['frame_id'])>drop+30 for e in events),'no IDR recovery')
            if name=='p_missing':require('ReferenceUncertain' in result['states'] and result['abandoned']>0,'gap did not flush pending input')
            if name=='late_display':
                late=[e for e in events if e['event']=='decoded_output' and e['display_timely']=='0']
                require(late and result['counts'].get('display_rejected',0)>0,'stall did not produce stale real images')
                require(any(e['event']=='input_accepted' and e['display_timely']=='0' for e in events),'late AU never accepted for reference')
                require(any(e['event']=='display_adopted' and e['generation']==old['generation'] and int(e['frame_id'])>int(old['frame_id']) for old in late for e in events),'no fresh dependent image after late reference input')
            if name=='late_transport':
                require(result['reassembled']>result['decoded'] and result['counts'].get('input_rejected',0)>0,'transport expiry did not separate completion and decode')
                require(result['recovery_deadline_snapshots']>0 and result['recognized']==result['control_used']==0,'expired transport images used')
            if name=='overtaken':
                delivered=[int(e['packet_id']) for e in rows(session/'uplink_link.csv') if e['event']=='delivered']
                require(any(a>b for a,b in zip(delivered,delivered[1:])),'overtaking not exercised')
                require(result['decoded']<result['reassembled'] and result['display_adopted']>0,'late/recovery stages indistinguishable')
            if budget:
                from verify_reach_budget import budget_audit
                item['budget']=budget_audit(session)
                require(result['recovery_deadline_snapshots']>0 and result['partial_chunk_snapshots']>0,'recovery deadline/missing chunks not observed')
                require(item['links']['uplink']['trace_dropped_packets']>0,'loss not exercised')
            item['passed']=True;save(out/'report.json',report);print(json.dumps(dict(case=name,**result)),flush=True)
        report['passed']=True
    except Exception as e:report['error']=str(e);print(str(e),file=sys.stderr)
    save(out/'report.json',report);render(out,report)
    return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
