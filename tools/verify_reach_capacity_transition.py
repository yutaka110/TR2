"""Prospective supplementary G2-06 case: isolate in-task capacity degradation.

The original 8 KiB queue case is retained: it fails initial random access.
This case fixes a 32 KiB queue BEFORE execution and is reported separately.
"""
import argparse
import subprocess
import sys
from pathlib import Path
from verify_reach_integration import direction, read, rows, save, sha, require, integration_audit, budget_audit, decode_audit, state_audit, render


def main():
    p = argparse.ArgumentParser(); p.add_argument('--name',required=True); p.add_argument('--build-name',default='reach_g2_feedback_20260922'); args = p.parse_args()
    for value in (args.name,args.build_name): require(value.replace('_','').replace('-','').isalnum(),'invalid name')
    repo = Path(__file__).resolve().parents[1]; root = repo/'artifacts'/args.build_name; out = root/'verification'/args.name; out.mkdir(parents=True,exist_ok=False)
    link = dict(uplink=direction(queue=32768),downlink=direction()); link['uplink']['capacity'] += [dict(at_us=2000000,bps=750000),dict(at_us=4000000,bps=6000000)]
    budget = read(repo/'config/reach_rt_g2_budget.json'); save(out/'link.json',link); save(out/'budget.json',budget)
    plan = dict(name='T2_capacity_transition',duration_s=60,task='T2',link=link,budget=budget,initial_y_m=0,initial_yaw_rad=0,
        executable_sha256=sha(root/'bin/Release/GE3.exe'),verifier_sha256=sha(repo/'tools/verify_reach_integration.py'),runner_sha256=sha(Path(__file__)),
        retries=0,reason='original 8192-byte queue prevented initial IDR decoding; independently preregister 32768 bytes to observe capacity transition after usable images',
        requirements=['valid full window and all audits','decoded image and motion before 2 seconds','tail drop during 2-4 second capacity reduction','task outcome retained even if timeout'])
    save(out/'plan.json',plan); report = dict(passed=False,complete=False,executable_sha256=plan['executable_sha256'],cases=[])
    case = dict(name=plan['name'],task='T2',passed=False); report['cases'].append(case)
    try:
        result = subprocess.run([sys.executable,str(repo/'tools/run_reach_g1.py'),'--stage','command_udp','--task','T2','--duration','60','--name',args.name,'--build-name',args.build_name,'--link-config',str(out/'link.json'),'--budget-config',str(out/'budget.json'),'--state-feedback','--packet-trace','--timeout','95'],cwd=repo,capture_output=True)
        (out/'stdout.txt').write_bytes(result.stdout); (out/'stderr.txt').write_bytes(result.stderr)
        invocation = root/'runs'/args.name; session = next((invocation/'sessions').iterdir()); case['session'] = str(session)
        require(result.returncode == 0,'native capacity trial failed')
        require(read(invocation/'launch.json')['executable_sha256'] == plan['executable_sha256'],'binary changed')
        case['integration'] = integration_audit(session,out/case['name']); case['budget'] = budget_audit(session); case['decode'] = decode_audit(session); case['state'] = state_audit(session)
        origin = read(session/'state_model.json')['origin_us']
        require(any(e['event']=='decoded_output' and int(e['event_us']) < origin+2000000 for e in rows(session/'decode_events.csv')),'no usable initial IDR before degradation')
        chain = rows(out/case['name']/'actuation_chain.csv')
        require(any(float(r['wall_s']) < 2 and float(r['v_m_s']) > .1 for r in chain),'robot not moving before degradation')
        require(any(r['event']=='tail_drop' and 2000000 <= int(r['arrival_us']) < 4000000 for r in rows(session/'uplink_link.csv')),'no in-task queue overflow')
        require(not case['integration']['allowance_violations'],'pose allowance violation')
        case['passed'] = report['passed'] = report['complete'] = True
    except Exception as error: case['error'] = str(error)
    save(out/'report.json',report); render(out,report); print(dict(passed=report['passed'],outcome=case.get('integration',{}).get('motion',{}).get('outcome'),error=case.get('error')))
    return 0 if report['passed'] else 1


if __name__ == '__main__': raise SystemExit(main())
