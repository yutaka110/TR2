"""Strict actual-native parser rejection for common IP-budget settings."""
import argparse,copy,hashlib,json,os,subprocess
from pathlib import Path


def main():
    p=argparse.ArgumentParser();p.add_argument('--name',required=True);p.add_argument('--build-name',default='reach_g2_budget_20260922');args=p.parse_args()
    for v in (args.name,args.build_name):
        if not v.replace('_','').replace('-','').isalnum():p.error('invalid name')
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/args.build_name;out=root/'verification'/args.name;out.mkdir(parents=True,exist_ok=False)
    read=lambda name:json.loads((repo/'config'/name).read_text());base=read('reach_rt_g1_command.json');base['link_model']=read('reach_rt_g2_link.json');base['ip_budget']=read('reach_rt_g2_budget.json')
    mutations=[('missing_link',lambda c:c.pop('link_model')),('missing_total',lambda c:c['ip_budget'].pop('total')),('unknown_top',lambda c:c['ip_budget'].update(typo=1)),('bad_fec',lambda c:c['ip_budget'].update(fec_group_chunks=3)),('boolean_fec',lambda c:c['ip_budget'].update(fec_group_chunks=True)),('wrong_stage',lambda c:c.update(stage='foundation'))]
    for field,value in [('rate_bps',-1),('rate_bps',1000000001),('rate_bps',.5),('rate_bps',True),('burst_ip_bytes',27),('burst_ip_bytes',16777217),('burst_ip_bytes',.5),('max_ip_bytes',-1),('max_ip_bytes',1000000000001),('max_ip_bytes',True)]:
        mutations.append((field+'_'+str(value),lambda c,f=field,v=value:c['ip_budget']['total'].update({f:v})))
    mutations.append(('unknown_limit',lambda c:c['ip_budget']['downlink'].update(extra=1)))
    startup=subprocess.STARTUPINFO();startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=subprocess.SW_HIDE
    exe=root/'bin/Release/GE3.exe';report=dict(passed=False,executable_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),cases=[])
    for name,mutate in mutations:
        folder=out/name;folder.mkdir();c=copy.deepcopy(base);mutate(c);path=folder/'input.json';path.write_text(json.dumps(c))
        env={k:v for k,v in os.environ.items() if not k.upper().startswith(('TR2_REACH_','TR2_RESEARCH_','RNVP_','TR2_NETWORK_'))};env.update(TR2_RESEARCH_MODE='reach_rt',TR2_REACH_HEADLESS='1',TR2_REACH_CONFIG=str(path),TR2_REACH_OUTPUT_ROOT=str(folder/'sessions'))
        r=subprocess.run([str(exe)],cwd=folder,env=env,startupinfo=startup,capture_output=True,timeout=10);(folder/'stderr.txt').write_bytes(r.stderr)
        report['cases'].append(dict(name=name,passed=r.returncode==2 and not (folder/'sessions').exists(),exit_code=r.returncode))
    report['passed']=all(c['passed'] for c in report['cases']);(out/'report.json').write_text(json.dumps(report,indent=2));print(json.dumps(dict(passed=report['passed'],cases=len(report['cases']))));return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
