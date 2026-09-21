"""Exercise a deliberately slow decoder observer without blocking robot timing."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

from verify_reach_closed_loop import read, rows, save
from verify_reach_command import audit, require


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--name",required=True)
    parser.add_argument("--build-name",default="reach_g1_stable_20260921")
    args=parser.parse_args()
    for value in (args.name,args.build_name):
        if not value.replace('_','').replace('-','').isalnum():parser.error('invalid name')
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/args.build_name
    output=root/'verification'/args.name;output.mkdir(parents=True,exist_ok=False)
    report=dict(passed=False,cases=[],executable_sha256=hashlib.sha256((root/'bin/Release/GE3.exe').read_bytes()).hexdigest())
    try:
        for name,options in [('slow_observer',['--diagnostic-recognition-delay-ms','250']),('software_stale',['--encoder','software'])]:
            run=args.name+'_'+name
            command=[sys.executable,str(repo/'tools/run_reach_g1.py'),'--stage','command_udp','--build-name',args.build_name,'--task','T2','--duration','4','--name',run,*options]
            result=subprocess.run(command,cwd=repo,capture_output=True)
            (output/(name+'.stdout.txt')).write_bytes(result.stdout);(output/(name+'.stderr.txt')).write_bytes(result.stderr)
            require(result.returncode==0,name+' execution failed')
            invocation=root/'runs'/run;session=next((invocation/'sessions').iterdir())
            require(read(invocation/'launch.json')['executable_sha256']==report['executable_sha256'],'binary changed')
            causal=audit(session);video=read(session/'robot_video_summary.json');metrics=video['pipeline_metrics']
            require(read(session/'summary.json')['clock_tick_counts']==dict(physics=400,camera=120,control=80),'clock blocked')
            require(video['matched']==120 and causal['summary']['statuses']=={'accepted':80},'incomplete pipeline')
            timing=rows(session/'encoder_timing.csv');decode=rows(session/'decode_timing.csv')
            require(len(timing)==len(decode)==120,'missing pipeline timings')
            require(metrics['max_capture_queue']<=8,'capture queue changed')
            if name=='slow_observer':
                slow=next(r for r in decode if r['frame_id']=='60')
                require(int(slow['recognition_us'])>=250000,'diagnostic did not run')
                require(metrics['max_publish_wait_us']<50000,'publication still blocked')
                commands=rows(session/'commands.csv')
                require(any(r['reason']=='observation_expired' and float(r['v_m_s'])==float(r['w_rad_s'])==0 for r in commands),'capture deadline did not stop control')
                require(any(int(r['source_frame_id'])>80 and float(r['v_m_s'])>0 for r in commands),'fresh image control did not resume')
            else:
                require(any(r['reason']=='stale_at_receive' for r in rows(session/'observations.csv')),'software buffering not observed')
                require(all(float(r['v_m_s'])==float(r['w_rad_s'])==0 for r in rows(session/'commands.csv')),'stale software images moved robot')
            report['cases'].append(dict(name=name,session=str(session),metrics=metrics,passed=True))
            print(json.dumps(report['cases'][-1]),flush=True)
        report['passed']=True
    except Exception as error:
        report['error']=str(error)
    save(output/'report.json',report)
    return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
