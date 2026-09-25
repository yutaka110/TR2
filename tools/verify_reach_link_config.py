"""Reject malformed link settings using the actual application parser."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--name',required=True);parser.add_argument('--build-name',default='reach_g2_link_20260922');args=parser.parse_args()
    for s in (args.name,args.build_name):
        if not s.replace('_','').replace('-','').isalnum():parser.error('invalid name')
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/args.build_name;out=root/'verification'/args.name;out.mkdir(parents=True,exist_ok=False)
    base=json.loads((repo/'config/reach_rt_g1_command.json').read_text());base['link_model']=json.loads((repo/'config/reach_rt_g2_link.json').read_text())
    mutations=[('small_queue',lambda x:x['link_model']['uplink'].update(queue_ip_bytes=27)),
      ('fractional_queue',lambda x:x['link_model']['uplink'].update(queue_ip_bytes=116.5)),
      ('empty_trace',lambda x:x['link_model']['uplink'].update(capacity=[])),
      ('non_array',lambda x:x['link_model']['uplink'].update(capacity={})),
      ('negative_rate',lambda x:x['link_model']['uplink']['capacity'][0].update(bps=-1)),
      ('excess_rate',lambda x:x['link_model']['uplink']['capacity'][0].update(bps=1000000001)),
      ('boolean_rate',lambda x:x['link_model']['uplink']['capacity'][0].update(bps=True)),
      ('nonzero_origin',lambda x:x['link_model']['uplink']['capacity'][0].update(at_us=1)),
      ('duplicate_time',lambda x:x['link_model']['uplink']['capacity'].append(dict(at_us=0,bps=1))),
      ('unknown_key',lambda x:x['link_model']['uplink'].update(bandwidth=1000)),
      ('missing_direction',lambda x:x['link_model'].pop('downlink')),
      ('mixed_diagnostic',lambda x:x['command_link'].update(scenario='late')),
      ('empty_impairment',lambda x:x['link_model']['uplink'].update(impairment=[])),
      ('non_array_impairment',lambda x:x['link_model']['uplink'].update(impairment={})),
      ('numeric_drop',lambda x:x['link_model']['uplink'].update(impairment=[dict(at_us=0,drop=1,delay_us=0)])),
      ('negative_delay',lambda x:x['link_model']['uplink'].update(impairment=[dict(at_us=0,drop=False,delay_us=-1)])),
      ('excess_delay',lambda x:x['link_model']['uplink'].update(impairment=[dict(at_us=0,drop=False,delay_us=1000001)])),
      ('fractional_delay',lambda x:x['link_model']['uplink'].update(impairment=[dict(at_us=0,drop=False,delay_us=.5)])),
      ('missing_drop',lambda x:x['link_model']['uplink'].update(impairment=[dict(at_us=0,delay_us=0)])),
      ('impairment_origin',lambda x:x['link_model']['uplink'].update(impairment=[dict(at_us=1,drop=False,delay_us=0)])),
      ('duplicate_impairment_time',lambda x:x['link_model']['uplink'].update(impairment=[dict(at_us=0,drop=False,delay_us=0)]*2)),
      ('excess_trace_points',lambda x:x['link_model']['uplink'].update(impairment=[dict(at_us=i,drop=False,delay_us=0) for i in range(4097)]))]
    exe=root/'bin/Release/GE3.exe';report=dict(passed=False,executable_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),cases=[])
    startup=subprocess.STARTUPINFO();startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=subprocess.SW_HIDE
    for name,mutate in mutations:
        config=copy.deepcopy(base);mutate(config);folder=out/name;folder.mkdir();path=folder/'input.json';path.write_text(json.dumps(config))
        env={k:v for k,v in os.environ.items() if not k.upper().startswith(('TR2_REACH_','TR2_RESEARCH_','RNVP_','TR2_NETWORK_'))}
        env.update(TR2_RESEARCH_MODE='reach_rt',TR2_REACH_HEADLESS='1',TR2_REACH_CONFIG=str(path),TR2_REACH_OUTPUT_ROOT=str(folder/'sessions'))
        r=subprocess.run([str(exe)],cwd=folder,env=env,startupinfo=startup,capture_output=True,timeout=10)
        (folder/'stderr.txt').write_bytes(r.stderr);passed=r.returncode==2 and not (folder/'sessions').exists()
        report['cases'].append(dict(name=name,passed=passed,exit_code=r.returncode))
    report['passed']=all(c['passed'] for c in report['cases']);(out/'report.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2));return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
