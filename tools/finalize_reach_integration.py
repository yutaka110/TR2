"""Close G2 from fixed evidence; preserve task failures and source provenance."""
import argparse
import copy
from collections import Counter
from pathlib import Path
import sys
import subprocess
from verify_reach_integration import integration_audit, read, rows, save, sha, require, render, scenarios, write_csv
from audit_reach_stage_conservation import stage_conservation


def main():
    parser = argparse.ArgumentParser(); parser.add_argument('--name',required=True); parser.add_argument('--supplement',required=True); parser.add_argument('--build-name',default='reach_g2_feedback_20260922'); args = parser.parse_args()
    for value in (args.name,args.supplement,args.build_name): require(value.replace('_','').replace('-','').isalnum(),'invalid name')
    repo = Path(__file__).resolve().parents[1]; root = repo/'artifacts'/args.build_name; out = root/'verification'/args.name
    plan = read(out/'plan.json'); report = read(out/'report.json'); executable = sha(root/'bin/Release/GE3.exe')
    require(report['passed'] and report['complete'] and len(report['cases']) == 8,'integration matrix incomplete or failed')
    require(executable == plan['executable_sha256'] == report['executable_sha256'],'exe hash changed')
    require(sha(repo/'tools/verify_reach_integration.py') == plan['verifier_sha256'],'verifier changed after preregistration')
    require([c['name'] for c in report['cases']] == [c['name'] for c in plan['cases']] == [c['name'] for c in scenarios()],'case selection changed')
    supplementary = root/'verification'/args.supplement; additional = read(supplementary/'report.json'); extra_plan = read(supplementary/'plan.json')
    require(additional['passed'] and additional['complete'] and len(additional['cases']) == 1,'supplementary capacity trial failed')
    require(additional['executable_sha256'] == extra_plan['executable_sha256'] == executable,'supplementary binary mismatch')
    require(extra_plan['verifier_sha256'] == plan['verifier_sha256'] and extra_plan['runner_sha256'] == sha(repo/'tools/verify_reach_capacity_transition.py'),'supplementary audit code changed')
    source_checks = 0; totals = Counter(); outcomes = Counter(); result = []
    trials = [(c,p,out,plan['budget']) for c,p in zip(report['cases'],plan['cases'])]
    trials += [(additional['cases'][0],extra_plan,supplementary,extra_plan['budget'])]
    for case, expected, case_root, expected_budget in trials:
        session = Path(case['session']); invocation = session.parent.parent; config = read(session/'config.effective.json')
        require(config['duration_s'] == 60 and config['task'] == expected['task'] and config['world']['initial_y_m'] == config['world']['initial_yaw_rad'] == 0,'trial differs from planned task/window')
        require(config['ip_budget'] == expected_budget and config['state_feedback'] is True,'budget or state mode changed')
        if 'link' in expected: require(config['link_model'] == expected['link'],'fixed trace differs from plan')
        else:
            from generate_reach_trace import load_bundle
            link, metadata = load_bundle(out/(case['name']+'_trace'),60000000)
            require(link == config['link_model'] and metadata['link_sha256'] == expected['trace_sha256'],'seeded trace differs from plan')
        require(read(invocation/'launch.json')['executable_sha256'] == executable,'launch exe hash')
        for relative, digest in read(invocation/'source_hashes.json').items():
            if relative.startswith(('research/','network/')) and Path(relative).suffix in ('.cpp','.h'):
                require(sha(repo/relative) == digest == sha(invocation/'research_source'/relative),'native source changed: '+relative); source_checks += 1
        require(integration_audit(session) == case['integration'],'cross-stage re-audit changed')
        for stage in ('integration','budget','decode','state'): require(case[stage]['passed'],stage+' failed')
        a = case['integration']; motion = a['motion']; outcomes[motion['outcome']] += 1
        conservation = stage_conservation(session)
        write_csv(case_root/case['name']/'frame_terminal.csv',conservation['frames'])
        totals['physics_steps'] += a['applications_joined']; totals['commands_replayed'] += a['controller_commands_replayed']; totals['frames_joined'] += a['frames_joined']; totals['sender_estimates'] += case['state']['causal_estimates_audited']; totals['ip_bytes'] += case['budget']['summary']['attempted_ip_bytes']
        chain = rows(case_root/case['name']/'actuation_chain.csv')
        # Stop commands also copy rejected image metadata. Such a tag is not a
        # control-used image; keep its age separate from the actual control AoI.
        used_commands = {int(e['command_sequence']) for e in rows(session/'decode_events.csv') if e['event']=='control_used'}
        used_ages = [(float(r['wall_s'])-float(r['capture_s']))*1000 for r in chain if r['application_reason']=='active' and int(r['command_sequence']) in used_commands]
        age_metrics = dict(source_tag_age_max_ms=a['control_loop_aoi_max_ms'],control_used_image_age_at_application_max_ms=max(used_ages,default=0),control_used_applications=len(used_ages))
        if expected.get('outage'):
            start, end = expected['outage']
            stop = next(e for e in a['stop_events'] if start < e['wall_s'] < end+.5 and not e['terminal'])
            stopped_tick = next(i for i,r in enumerate(chain) if float(r['wall_s']) >= stop['wall_s'])
            resume = next(r for r in chain[stopped_tick+1:] if float(r['v_m_s']) > .001)
            transitions = dict(outage_s=[start,end],stop=stop,resume_wall_s=float(resume['wall_s']),resume_source_frame=int(resume['source_frame_id']),resume_capture_s=float(resume['capture_s']))
            require(transitions['resume_capture_s'] >= end if 'uplink' in case['name'] else float(resume['command_generated_s']) >= end-.1,'recovery used stale input')
        else: transitions = None
        result.append(dict(name=case['name'],task=case['task'],outcome=motion['outcome'],terminal=motion['terminal'],
            ip_bytes=case['budget']['summary']['attempted_ip_bytes'],decoded=case['decode']['decoded'],recognized=case['decode']['recognized'],
            queue_drops=case['budget']['uplink']['tail_dropped_packets']+case['budget']['downlink']['tail_dropped_packets'],
            trace_drops=case['budget']['uplink']['trace_dropped_packets']+case['budget']['downlink']['trace_dropped_packets'],
            application_reasons=a['application_reasons'],allowance_violations=a['allowance_violations'],outage=transitions,age_metrics=age_metrics,frame_terminal_counts=conservation['counts']))
    mutation = subprocess.run([sys.executable,str(repo/'tools/test_reach_integration_audit.py'),report['cases'][0]['session'],'--output',str(out/'auditor_fault_tests.json')],cwd=repo,capture_output=True)
    (out/'auditor_fault_tests.stdout.txt').write_bytes(mutation.stdout); (out/'auditor_fault_tests.stderr.txt').write_bytes(mutation.stderr)
    require(mutation.returncode == 0 and read(out/'auditor_fault_tests.json')['passed'],'auditor mutation tests failed')
    conservation_test = subprocess.run([sys.executable,str(repo/'tools/test_reach_stage_conservation.py'),str(out/'report.json')],cwd=repo,capture_output=True)
    (out/'stage_conservation_fault_tests.stdout.txt').write_bytes(conservation_test.stdout)
    require(conservation_test.returncode == 0 and read(out/'stage_conservation_fault_tests.json')['passed'],'stage conservation mutation tests failed')
    historical = []
    for build, name in [('reach_g2_link_20260922','acceptance_final_02'),('reach_g2_trace_20260922','acceptance_01'),('reach_g2_budget_20260922','acceptance_02'),('reach_g2_decode_20260922','acceptance_final_02'),('reach_g2_feedback_20260922','acceptance_01')]:
        path = repo/'artifacts'/build/'verification'/name/'task_decision.json'; data = read(path)
        require(data['passed'],'prior task incomplete')
        historical.append(dict(task=data['task'],path=str(path.relative_to(repo)),sha256=sha(path),executable_sha256=data['executable_sha256']))
    # G2-05's exact runtime already passed native units, decoder and legacy modes.
    # Qualify their reuse by matching native source snapshots as well as binary.
    previous = root/'verification/acceptance_01'; evidence = read(previous/'evidence_audit.json')
    require(evidence['passed'] and evidence['executable_sha256'] == executable,'same-binary regression evidence unavailable')
    prior_session = Path(read(previous/'report.json')['cases'][0]['session']); previous_sources = read(prior_session.parent.parent/'source_hashes.json')
    for relative,digest in previous_sources.items():
        if relative.startswith(('research/','network/')) and Path(relative).suffix in ('.cpp','.h'):
            require(sha(repo/relative) == digest,'G2-05 native source differs'); source_checks += 1
    for name in ('units_final_01','decode_regression_01','legacy_regression_01'):
        require(read(root/'verification'/name/'report.json')['passed'],'reused regression failed')
    # Freeze the actual offline evaluators too, including modules imported by them.
    snapshot = out/'auditor_source'; snapshot.mkdir(exist_ok=True); hashes = {}
    for path in (repo/'tools').glob('*reach*.py'):
        (snapshot/path.name).write_bytes(path.read_bytes()); hashes[path.name] = sha(path)
    save(out/'auditor_source_hashes.json',hashes)
    save(out/'aggregate.json',dict(cases=result,outcomes=dict(outcomes),totals=dict(totals),supplementary_report=str(supplementary/'report.json'),
        metric_note='initial report control_loop_aoi_max_ms includes rejected source tags on stop commands; use aggregate age_metrics.control_used_image_age_at_application_max_ms for actual control-use AoI'))
    decision = dict(task='G2-06',passed=True,status='complete',gate='G2',g2_gate_passed=True,executable_sha256=executable,
        native_snapshot_hash_checks=source_checks,checks=dict(full_windows=True,all_stage_audits=True,controller_replay=True,world_outcome_replay=True,
        causal_received_state=True,common_ip_budget=True,mutation_tests=True,frame_terminal_partition=True,stage_conservation_fault_tests=True,same_runtime_regressions=True),
        prior_task_evidence=historical,totals=dict(totals),outcomes=dict(outcomes),
        progress=dict(g2_completed=6,g2_total=6,overall_completed=19,overall_total=38,gates_passed=3,gates_total=7),next_task='G3-01',
        scope='fixed synthetic single-PC integration qualification; not policy superiority, statistical task success probability or full G1 pose revalidation')
    decision['supplementary_evidence'] = dict(plan=str(supplementary/'plan.json'),plan_sha256=sha(supplementary/'plan.json'),report=str(supplementary/'report.json'),report_sha256=sha(supplementary/'report.json'))
    save(out/'gate_decision.json',decision)
    display_report = copy.deepcopy(report)
    for case, metrics in zip(display_report['cases'],result):
        case['integration'].pop('control_loop_aoi_max_ms'); case['integration']['age_metrics'] = metrics['age_metrics']
    render(out,display_report)
    page = out/'index.html'; content = page.read_text(encoding='utf-8')
    content = content.replace('</h1>','</h1><p><b>G2ゲート：PASS</b> — 元の8条件と追加容量試験1条件を合計。<a href="../'+args.supplement+'/index.html">追加試験</a> ／ <a href="gate_decision.json">ゲート判定</a> ／ <a href="aggregate.json">全9件の集計</a></p>',1)
    page.write_text(content,encoding='utf-8'); print(decision['totals']); print(decision['outcomes'])


if __name__ == '__main__': main()
