"""G2-01 independent service-integral audit of real UDP relay records."""
import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import sys
from verify_reach_closed_loop import read,rows,save
from verify_reach_command import audit,require


def finish_time(start,ip_bytes,trace):
    remaining=ip_bytes*8*1000000
    at=start
    for i,p in enumerate(trace):
        end=trace[i+1]['at_us'] if i+1<len(trace) else 3601000000
        if end<=at:continue
        begin=max(at,p['at_us']);rate=p['bps'];available=rate*(end-begin)
        if rate and remaining<=available:return begin+(remaining+rate-1)//rate
        remaining-=available;at=end
    return math.inf


def link_audit(session,direction):
    summary=read(session/(direction+'_link_summary.json'));data=rows(session/(direction+'_link.csv'))
    offered=[r for r in data if r['event'] in ('admitted','tail_drop')]
    delivered={int(r['packet_id']):r for r in data if r['event']=='delivered'}
    cancelled={int(r['packet_id']):r for r in data if r['event']=='cancelled_at_close'}
    active=[];last_end=0;admitted={};max_occupied=0;crossed=0
    for i,r in enumerate(offered,1):
        require(int(r['packet_id'])==i,'arrival sequence')
        now=int(r['arrival_us']);b=int(r['ip_bytes'])
        require(b==len(bytes.fromhex(r['wire_hex']))+28,'IPv4/UDP service length')
        active=[x for x in active if x[0]>now];occupied=sum(x[1] for x in active)
        allowed=occupied+b<=summary['queue_limit_ip_bytes']
        require((r['event']=='admitted')==allowed,'incorrect tail-drop decision')
        if allowed:
            start=max(now,last_end);end=finish_time(start,b,summary['capacity_trace']);last_end=end
            active.append((end,b));admitted[i]=(r,start,end);occupied+=b
            if any(start<p['at_us']<end for p in summary['capacity_trace']):crossed+=1
            if i in delivered:
                d=delivered[i]
                require(int(d['start_us'])==start and int(d['completion_us'])==end,'serialization differs from capacity integral')
                require(int(d['actual_send_us'])>=end,'UDP sent before serialization complete')
            else:require(i in cancelled,'admitted packet disappeared')
        require(occupied==int(r['occupied_ip_bytes']),'incorrect queue occupancy')
        max_occupied=max(max_occupied,occupied)
    require(set(delivered)|set(cancelled)==set(admitted) and not set(delivered)&set(cancelled),'terminal packet partition')
    require(len(offered)==summary['offered_packets'] and len(admitted)==summary['admitted_packets'],'summary admissions')
    require(len(delivered)==summary['delivered_packets'] and len(cancelled)==summary['cancelled_packets'],'summary completions')
    require(summary['offered_packets']==summary['admitted_packets']+summary['tail_dropped_packets'],'packet conservation')
    require(summary['offered_ip_bytes']==sum(int(r['ip_bytes']) for r in offered),'offered bytes')
    require(summary['offered_ip_bytes']==summary['delivered_ip_bytes']+summary['tail_dropped_ip_bytes']+summary['cancelled_ip_bytes'],'byte conservation')
    require(max_occupied==summary['maximum_occupied_ip_bytes'] and max_occupied<=summary['queue_limit_ip_bytes'],'queue maximum')
    require(not summary['error'],'relay execution error')
    if direction=='uplink':
        def identity(row):
            b=bytes.fromhex(row['wire_hex']);return (int.from_bytes(b[8:12],'big'),int.from_bytes(b[16:20],'big'),int.from_bytes(b[20:22],'big'),len(b))
        trace=(session.parent.parent/'stderr.txt').read_text(encoding='utf-8',errors='replace')
        tx=Counter(tuple(map(int,m)) for m in re.findall(r'packet_tx,(\d+),(\d+),(\d+),(\d+)',trace))
        rx=Counter(tuple(map(int,m)) for m in re.findall(r'packet_rx,(\d+),(\d+),(\d+),(\d+)',trace))
        require(tx==Counter(identity(r) for r in offered),'video ingress mismatch before modeled queue')
        require(rx==Counter(identity(admitted[i][0]) for i in delivered),'video egress mismatch after modeled queue')
    return dict(**summary,cross_capacity_packets=crossed)


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--name',required=True);parser.add_argument('--build-name',default='reach_g2_link_20260922');args=parser.parse_args()
    for value in (args.name,args.build_name):
        if not value.replace('_','').replace('-','').isalnum():parser.error('invalid name')
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/args.build_name;out=root/'verification'/args.name;out.mkdir(parents=True,exist_ok=False)
    def cfg(rate,queue=16384,extra=()):return dict(queue_ip_bytes=queue,capacity=[dict(at_us=0,bps=rate),*extra])
    cases=[
        ('wide',cfg(6000000,65536),cfg(500000)),
        ('uplink_pause',cfg(3000000,16384,[dict(at_us=1500000,bps=0),dict(at_us=2200000,bps=6000000)]),cfg(500000)),
        ('downlink_pause',cfg(6000000,65536),cfg(500000,232,[dict(at_us=3000000,bps=0),dict(at_us=4000000,bps=500000)])),
        ('uplink_overflow',cfg(64000,4096),cfg(500000)),
        ('downlink_mid_packet',cfg(6000000,65536),cfg(1000,232,[dict(at_us=500000,bps=2000)])),
    ]
    report=dict(passed=False,executable_sha256=hashlib.sha256((root/'bin/Release/GE3.exe').read_bytes()).hexdigest(),cases=[])
    try:
        for name,up,down in cases:
            config=out/(name+'.json');save(config,dict(uplink=up,downlink=down));run=args.name+'_'+name
            duration=6 if name=='downlink_pause' else 4
            r=subprocess.run([sys.executable,str(repo/'tools/run_reach_g1.py'),'--stage','command_udp','--build-name',args.build_name,'--task','T2','--duration',str(duration),'--link-config',str(config),'--packet-trace','--name',run],cwd=repo,capture_output=True)
            (out/(name+'.stdout.txt')).write_bytes(r.stdout);(out/(name+'.stderr.txt')).write_bytes(r.stderr)
            invocation=root/'runs'/run;session=next((invocation/'sessions').iterdir())
            item=dict(name=name,session=str(session),duration_s=duration,passed=False);report['cases'].append(item)
            require(r.returncode==0,'app failed: '+str(read(session/'summary.json')))
            require(read(invocation/'launch.json')['executable_sha256']==report['executable_sha256'],'binary changed')
            require(read(session/'summary.json')['clock_tick_counts']==dict(physics=duration*100,camera=duration*30,control=duration*20),'clock interval')
            up_result=link_audit(session,'uplink');down_result=link_audit(session,'downlink');causal=audit(session,link_loss=True)
            require(up_result['origin_us']==down_result['origin_us'],'different directional time origins')
            item.update(uplink=up_result,downlink=down_result,command=causal)
            commands=rows(session/'commands.csv');world=rows(session/'world.csv');applied=rows(session/'udp_applied_commands.csv')
            if name=='wide':require(read(session/'robot_video_summary.json')['matched']==120 and down_result['delivered_packets']==80,'wide link incomplete')
            if name=='uplink_pause':require(up_result['tail_dropped_packets']>0 and down_result['tail_dropped_packets']==0,'directions not independent')
            if name=='downlink_pause':
                require(down_result['tail_dropped_packets']>0 and up_result['tail_dropped_packets']==0,'downlink overflow absent')
                require(any(r['reason']=='watchdog_timeout' for r in applied),'command outage did not trip watchdog')
                require(any(float(r['simulation_s'])<3 and float(r['v_m_s'])>.25 for r in world),'robot not moving before outage')
                require(any(3.7<float(r['simulation_s'])<4 and float(r['v_m_s'])==0 for r in world),'robot did not brake before link recovery')
                require(any(float(r['simulation_s'])>4.5 and float(r['v_m_s'])>.1 for r in world),'robot did not recover after fresh command')
            if name=='uplink_overflow':require(up_result['tail_dropped_packets']>0,'no tail drops')
            if name=='downlink_mid_packet':
                require(down_result['cross_capacity_packets']>0,'no packet spanned capacity transition')
                require(causal['summary']['statuses'].get('expired_on_arrival',0)>0 and all(float(r['v_m_s'])==0 for r in world),'late command moved robot')
            item['passed']=True;save(out/'report.json',report);print(json.dumps(dict(case=name,passed=True,up_packets=up_result['offered_packets'],up_drop=up_result['tail_dropped_packets'],down_drop=down_result['tail_dropped_packets'],decoded=causal['decoded_images'])),flush=True)
        report['passed']=True
    except Exception as e:report['error']=str(e);print(str(e),file=sys.stderr)
    save(out/'report.json',report);return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
