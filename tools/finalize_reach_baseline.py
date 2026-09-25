"""Qualify G3-01 with explicit invalid-run accounting and a separate paired supplement."""
from collections import Counter
from pathlib import Path
import argparse
import hashlib
import json
import shutil
from verify_reach_closed_loop import read, rows, save
from verify_reach_command import require


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p=argparse.ArgumentParser();p.add_argument('--name',default='study_01');p.add_argument('--build-name',default='reach_g3_baseline_v3_20260922');args=p.parse_args()
    for s in (args.name,args.build_name):require(s.replace('_','').replace('-','').isalnum(),'unsafe name')
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/args.build_name;out=root/'verification'/args.name
    report=read(out/'report.json');plan=read(out/'plan.json');lock=read(out/'baseline_lock.json');selection=read(out/'selection.json')
    require(lock['version']=='G3-01-v3','queue semantics and bounded probing are required')
    require(report['complete'] and len(report['trials'])==20,'original study incomplete')
    invalid=[r for r in report['trials'] if not r['passed']]
    require(len(invalid)==1 and invalid[0]['id']=='valid_T2_normal_B0_0p1','unexpected original invalid set')
    invalid_summary=read(Path(invalid[0]['session'])/'summary.json')
    require(invalid_summary['status']=='invalid' and invalid_summary['reason']=='camera deadline missed; historical pixels cannot be reconstructed','unexplained invalid trial')
    supplemental_dir=root/'verification/supplement_01';supplement=read(supplemental_dir/'report.json');supp_plan=read(supplemental_dir/'plan.json')
    require(supplement['passed'] and supplement['complete'] and len(supplement['trials'])==2,'supplement incomplete')
    require(supp_plan['original_report_sha256']==sha(out/'report.json') and supp_plan['selection_sha256']==sha(out/'selection.json'),'supplement did not preserve original evidence')
    require(supp_plan['executable_sha256']==report['executable_sha256'],'supplement binary differs')
    require(all(r['started_at']>=supp_plan['created_at'] and r['case']=='valid_T2_normal' and r['supplemental'] for r in supplement['trials']),'supplement not postregistered or wrong case')
    require(sha(out/'baseline_lock.json')==plan['lock_sha256']==selection['lock_sha256'],'baseline lock changed')
    require(sha(out/'plan.json')==selection['plan_sha256'],'plan changed after selection')
    require(sha(root/'bin/Release/GE3.exe')==report['executable_sha256']==lock['executable_sha256'],'executable changed')
    require(all(sha(repo/k)==v for k,v in lock['native_source_sha256'].items()),'native source changed')
    selected=min(selection['training_scores'],key=lambda r:(-r['successes'],r['terminal_seconds'],r['attempted_ip_bytes'],plan['lambdas'].index(r['lambda'])))
    require(selection['lambda']==selected['lambda'] and not selection['holdout_used'],'invalid selection')
    training=[r for r in report['trials'] if r['phase']=='training']
    require(all(r['passed'] for r in training),'invalid training')
    unpaired=[r for r in report['trials'] if r['case']=='valid_T2_normal' and r['passed']]
    require(len(unpaired)==1 and unpaired[0]['mode']=='B1','unexpected unpaired evidence')
    validation=[r for r in report['trials'] if r['phase']=='validation' and r['case']!='valid_T2_normal']+supplement['trials']
    all_trials=report['trials']+supplement['trials'];audited=[r for r in all_trials if r['passed']]
    require(len(training)==12 and len(validation)==8,'phase counts')
    for trial in all_trials:
        snapshot=read(Path(trial['session']).parent.parent/'source_hashes.json')
        for name in ('run_reach_g1.py','run_reach_baseline_study.py','verify_reach_baseline.py','verify_reach_integration.py','verify_reach_budget.py','verify_reach_decode.py','verify_reach_state.py','audit_reach_stage_conservation.py'):
            require(snapshot['tools/'+name]==sha(repo/'tools'/name),'measurement code changed during study: '+name)
    for weight in plan['lambdas']:
        entries=[r for r in training if r['mode']=='B1' and r['lambda']==weight]
        require(len(entries)==2,'tuning opportunity mismatch')
        score=next(s for s in selection['training_scores'] if s['lambda']==weight)
        require(score['successes']==sum(r['integration']['motion']['outcome']=='success' for r in entries),'selection success score')
        require(abs(score['terminal_seconds']-sum(r['integration']['motion']['terminal']['simulation_s'] if r['integration']['motion']['outcome']=='success' else 60 for r in entries))<1e-8,'selection time score')
        require(score['attempted_ip_bytes']==sum(r['budget']['summary']['attempted_ip_bytes'] for r in entries),'selection byte score')
    for c in (c for c in plan['cases'] if c['phase']=='validation'):
        pair=[r for r in validation if r['case']==c['name']];require(len(pair)==2 and {r['mode'] for r in pair}=={'B0','B1'},'missing pair')
        normalized=[]
        for r in pair:
            require(r['started_at']>selection['frozen_at'] and r['passed'],'holdout inspected before freeze')
            cfg=read(Path(r['session'])/'config.effective.json');cfg.pop('baseline');normalized.append(cfg)
            require(r['mode']=='B0' or r['lambda']==selection['lambda'],'validation retuning')
        require(normalized[0]==normalized[1],'paired conditions differ')
    units=read(root/'verification/units_01/report.json');require(units['passed'] and len(units['tests'])==7,'unit tests incomplete')
    for relative,digest in units['source_hashes'].items():
        require(sha(repo/relative)==digest,'unit dependency changed: '+relative)
    require(read(out/'auditor_fault_tests.json')['passed'],'auditor mutations failed')
    for kind in ('decode','state','legacy'):
        r=read(root/'verification'/(kind+'_regression_01')/'report.json');require(r['passed'],'regression '+kind)
        if 'executable_sha256' in r:require(r['executable_sha256']==report['executable_sha256'],'regression executable mismatch')
        for c in r.get('cases',[]):
            session=Path(c['session']);require(read(session.parent.parent/'launch.json')['executable_sha256']==report['executable_sha256'],'regression launch changed')
    disposition=dict(original_trials=20,supplemental_trials=2,full_valid_windows=len(audited),invalid_windows=1,
        invalid_trial=invalid[0]['id'],invalid_reason=invalid_summary,unpaired_valid_original=unpaired[0]['id'],
        selected_comparison='12 training + 6 original paired validation + both supplemental normal-T2 trials',
        scope='Original report remains failed, never overwritten. Aggregate counts/costs include all 21 fully audited windows; invalid partial data remain in its session, excluded from performance comparisons.')
    save(out/'trial_disposition.json',disposition)
    qualified=dict(report,passed=True,trials=training+validation,qualification_basis=disposition,
        original_report='report.json',supplemental_report='../supplement_01/report.json',
        outcomes=dict(Counter(r['integration']['motion']['outcome'] for r in training+validation)))
    save(out/'qualified_report.json',qualified)
    aggregate=dict(trials=len(all_trials),fully_audited_windows=len(audited),invalid_windows=1,training_trials=len(training),paired_validation_trials=len(validation),extra_unpaired_valid_trial=1,
        captures=sum(r['integration']['frames_joined'] for r in audited),
        commands=sum(r['integration']['controller_commands_replayed'] for r in audited),
        physics_steps=sum(r['integration']['applications_joined'] for r in audited),
        candidate_rows=sum(r['baseline']['candidate_rows'] for r in audited),
        baseline_decisions=sum(r['baseline']['decisions'] for r in audited),
        empty_queue_with_historical_delay=sum(r['baseline']['empty_queue_with_historical_delay'] for r in audited),
        idle_probe_offers=sum(r['baseline']['idle_probe_offers'] for r in audited),
        ip_bytes=sum(r['budget']['summary']['attempted_ip_bytes'] for r in audited),
        validation_outcomes={mode:dict(Counter(r['integration']['motion']['outcome'] for r in validation if r['mode']==mode)) for mode in ('B0','B1')},
        pose_allowance_violations=sum(len(r['integration']['allowance_violations']) for r in audited))
    offer_accounting=[]
    for trial in audited:
        session=Path(trial['session']);decisions=rows(session/'baseline_decisions.csv')
        probe_frames={int(d['frame_id']) for d in decisions if d['probe']=='1' and d['defer']=='0'}
        amount=0
        for packet in rows(session/'ip_budget.csv'):
            if packet['direction']!='uplink' or not int(packet['attempted_ip_bytes']):continue
            b=bytes.fromhex(packet['wire_hex'])
            if b[:4]==b'RNVP' and b[5] in (0,6) and int.from_bytes(b[16:20],'big') in probe_frames:amount+=int(packet['attempted_ip_bytes'])
        offer_accounting.append(dict(trial=trial['id'],frames=len(decisions),offered=sum(d['defer']=='0' for d in decisions),
            deferred=sum(d['defer']=='1' for d in decisions),probe_offered_frames=len(probe_frames),probe_frame_uplink_ip_bytes=amount,
            scope='actual charged video/FEC/repair packets for probe frames; feedback is charged in common totals, not attributed here; legacy video sent counter is not actual network delivery'))
    save(out/'frame_offer_accounting.json',offer_accounting)
    aggregate['probe_frame_uplink_ip_bytes']=sum(r['probe_frame_uplink_ip_bytes'] for r in offer_accounting)
    save(out/'aggregate.json',aggregate)
    source=out/'audit_source';source.mkdir(exist_ok=True)
    for path in (repo/'tools').glob('*.py'):
        if 'reach' in path.name:shutil.copy2(path,source/path.name)
    evidence={str(f.relative_to(repo)).replace('\\','/'):sha(f) for f in source.glob('*.py')}
    for f in ('plan.json','selection.json','baseline_lock.json','report.json','qualified_report.json','trial_disposition.json','aggregate.json','auditor_fault_tests.json','frame_offer_accounting.json'):
        evidence[str((out/f).relative_to(repo)).replace('\\','/')]=sha(out/f)
    for f in ('plan.json','report.json'):
        evidence[str((supplemental_dir/f).relative_to(repo)).replace('\\','/')]=sha(supplemental_dir/f)
    save(out/'evidence_audit.json',dict(passed=True,executable_sha256=report['executable_sha256'],source_files=len(lock['native_source_sha256']),files=evidence))
    save(out/'task_decision.json',dict(passed=True,task='G3-01',g3_completed=1,g3_total=5,all_completed=20,all_total=38,gates_passed=3,gates_total=7,
        selected_lambda=selection['lambda'],next='G3-02 B2/B3 information ablation',
        scope='fixed B0 and initially tuned RNVP/XOR B1; no G3 gate, research superiority or statistical qualification claimed',aggregate=aggregate))
    print(json.dumps(aggregate,indent=2))


if __name__=='__main__':main()
