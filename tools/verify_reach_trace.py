"""Real H.264/RCMD trials against fixed exogenous time traces (G2-02)."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from generate_reach_trace import write_bundle, encoded
from verify_reach_link import link_audit
from verify_reach_command import audit, require
from verify_reach_closed_loop import read, rows, save


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--name',required=True);parser.add_argument('--build-name',default='reach_g2_trace_20260922');args=parser.parse_args()
    for v in (args.name,args.build_name):
        if not v.replace('_','').replace('-','').isalnum():parser.error('invalid name')
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/args.build_name;out=root/'verification'/args.name;out.mkdir(parents=True,exist_ok=False)
    bundle=out/'paired_trace';frozen,meta=write_bundle(bundle,horizon_us=6000000)
    def cfg(delay=0,points=None):return dict(queue_ip_bytes=65536,capacity=[dict(at_us=0,bps=6000000)],impairment=points or [dict(at_us=0,drop=False,delay_us=delay)])
    blackout=dict(uplink=cfg(),downlink=cfg(points=[dict(at_us=0,drop=False,delay_us=1000),dict(at_us=3000000,drop=True,delay_us=1000),dict(at_us=4000000,drop=False,delay_us=1000)]))
    reorder=dict(uplink=cfg(),downlink=cfg(points=[dict(at_us=0,drop=False,delay_us=250000),dict(at_us=2000000,drop=False,delay_us=0)]))
    shutdown=dict(uplink=cfg(),downlink=cfg(1000000))
    cases=[('paired_T1','T1',6,None,0),('paired_T2','T2',6,None,.15),('command_blackout','T2',6,blackout,0),('delay_reorder','T2',4,reorder,0),('propagation_close','T2',.5,shutdown,0)]
    report=dict(passed=False,executable_sha256=hashlib.sha256((root/'bin/Release/GE3.exe').read_bytes()).hexdigest(),bundle_trace_id=meta['link_sha256'],cases=[])
    try:
        for name,task,duration,config,y in cases:
            if config is None:options=['--trace-bundle',str(bundle)];config=frozen
            else:
                path=out/(name+'.json');save(path,config);options=['--link-config',str(path)]
            run=args.name+'_'+name
            result=subprocess.run([sys.executable,str(repo/'tools/run_reach_g1.py'),'--stage','command_udp','--build-name',args.build_name,'--task',task,'--duration',str(duration),'--initial-y',str(y),'--packet-trace','--name',run,*options],cwd=repo,capture_output=True)
            (out/(name+'.stdout.txt')).write_bytes(result.stdout);(out/(name+'.stderr.txt')).write_bytes(result.stderr)
            invocation=root/'runs'/run;session=next((invocation/'sessions').iterdir());item=dict(name=name,session=str(session),duration_s=duration,passed=False);report['cases'].append(item)
            require(result.returncode==0,'native trial failed: '+str(read(session/'summary.json')))
            require(read(invocation/'launch.json')['executable_sha256']==report['executable_sha256'],'exe changed')
            require(read(session/'summary.json')['clock_tick_counts']==dict(physics=round(duration*100),camera=round(duration*30),control=round(duration*20)),'clock count')
            links={d:link_audit(session,d) for d in ('uplink','downlink')};item.update(links);item['command']=audit(session,link_loss=True)
            require(links['uplink']['origin_us']==links['downlink']['origin_us'],'different directional clocks')
            for d in links:
                trace=read(session/(d+'_trace.json'));require(trace=={k:config[d][k] for k in ('capacity','impairment')},'runtime trace differs from input')
                require(links[d]['trace_sha256']==hashlib.sha256(encoded(trace)).hexdigest(),'canonical trace hash')
                require(read(session/'config.effective.json')['link_model'][d]==config[d],'effective config changed')
            if name.startswith('paired'):
                require(read(invocation/'launch.json')['exogenous_trace']==meta,'bundle metadata differs')
                require(all(links[d]['trace_dropped_packets']>0 for d in links),'paired trace never exercised loss')
            if name=='command_blackout':
                world=rows(session/'world.csv');applied=rows(session/'udp_applied_commands.csv')
                require(links['downlink']['trace_dropped_packets']>0 and links['downlink']['tail_dropped_packets']==0,'blackout confused with queue loss')
                require(any(float(r['simulation_s'])<3 and float(r['v_m_s'])>.25 for r in world),'robot not moving before loss')
                require(any(r['reason']=='watchdog_timeout' for r in applied),'blackout did not trigger watchdog')
                require(any(3.7<float(r['simulation_s'])<4 and float(r['v_m_s'])==0 for r in world),'robot failed to stop in blackout')
                require(any(float(r['simulation_s'])>4.5 and float(r['v_m_s'])>.1 for r in world),'robot failed to resume')
            if name=='delay_reorder':
                delivery=[int(r['packet_id']) for r in rows(session/'downlink_link.csv') if r['event']=='delivered']
                require(any(a>b for a,b in zip(delivery,delivery[1:])),'delay reduction did not cause overtaking')
                require(item['command']['summary']['statuses'].get('expired_on_arrival',0)>0,'long delays not rejected')
            if name=='propagation_close':require(links['downlink']['propagation_cancelled_packets']==10 and links['downlink']['delivered_packets']==0,'shutdown did not classify in-flight packets')
            item['passed']=True;save(out/'report.json',report);print(json.dumps(dict(case=name,passed=True,up_packets=links['uplink']['offered_packets'],up_loss=links['uplink']['trace_dropped_packets'],down_loss=links['downlink']['trace_dropped_packets'])),flush=True)
        a,b=report['cases'][:2]
        require(a['uplink']['offered_packets']!=b['uplink']['offered_packets'],'paired input packet counts unexpectedly equal')
        require(all(a[d]['trace_sha256']==b[d]['trace_sha256'] for d in ('uplink','downlink')),'paired exogenous states differ')
        report['fairness']=dict(passed=True,packet_counts=[a['uplink']['offered_packets'],b['uplink']['offered_packets']],same_directional_trace_hashes=True,scope='same external time conditions; outcomes and queue occupancy may differ')
        report['passed']=True
    except Exception as e:report['error']=str(e);print(str(e),file=sys.stderr)
    save(out/'report.json',report);return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
