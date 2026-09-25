"""Validate selection, paired conditions, provenance, regressions and all outcomes."""
import argparse,json,math,shutil
from collections import Counter
from pathlib import Path
from run_reach_ablation_study import sha,stamp
from verify_reach_closed_loop import read,save
from verify_reach_command import require


def main():
    p=argparse.ArgumentParser();p.add_argument('--name',default='study_01');p.add_argument('--build-name',default='reach_g3_ablation_v5_20260923');a=p.parse_args()
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/a.build_name;out=root/'verification'/a.name
    r=read(out/'report.json');plan=read(out/'plan.json');lock=read(out/'baseline_lock.json');selection=read(out/'selection.json')
    require(r['passed'] and r['complete'] and len(r['trials'])==42,'study incomplete or invalid')
    require(sha(out/'baseline_lock.json')==plan['lock_sha256'] and sha(out/'plan.json')==selection['plan_sha256'],'plan/selection changed')
    require(not selection['holdout_used'],'holdout used for selection')
    require(sha(root/'bin/Release/GE3.exe')==lock['executable_sha256']==r['executable_sha256'],'binary changed')
    for f,h in lock['source_sha256'].items():require(sha(repo/f)==h,'fixed source changed: '+f)
    for t in r['trials']:
        require(t['passed'],'invalid trial excluded');session=Path(t['session']);inv=session.parent.parent;launch=read(inv/'launch.json')
        require(launch['executable_sha256']==r['executable_sha256'] and launch['arguments']['process_priority']=='above_normal' and launch['arguments']['precise_wait'] is True,'execution conditions differ')
        hs=read(inv/'source_hashes.json');require(all(hs[k]==v for k,v in lock['source_sha256'].items()),'snapshot changed')
    validation=[t for t in r['trials'] if t['phase']=='validation'];training=[t for t in r['trials'] if t['phase']=='training']
    require(len(training)==30 and len(validation)==12,'phase counts')
    for mode in plan['modes']:
        scores=[]
        for w in plan['lambdas']:
            ts=[t for t in training if t['mode']==mode and t['lambda']==w];require(len(ts)==2,'unequal tuning budget')
            scores.append(dict(**{'lambda':w},successes=sum(t['integration']['motion']['outcome']=='success' for t in ts),terminal_seconds=sum(t['integration']['motion']['terminal']['simulation_s'] if t['integration']['motion']['outcome']=='success' else 60 for t in ts),attempted_ip_bytes=sum(t['budget']['summary']['attempted_ip_bytes'] for t in ts)))
        require(scores==selection[mode]['training_scores'],'scores do not match trials')
        best=min(scores,key=lambda s:(-s['successes'],s['terminal_seconds'],s['attempted_ip_bytes'],plan['lambdas'].index(s['lambda'])))
        require(best['lambda']==selection[mode]['lambda'],'nonoptimal coefficient selected')
    for case in plan['cases']:
        if case['phase']!='validation':continue
        ts=[t for t in validation if t['case']==case['name']];require(len(ts)==3 and {t['mode'] for t in ts}==set(plan['modes']),'missing comparison member')
        configs=[]
        for t in ts:
            require(t['started_at']>selection['frozen_at'] and t['lambda']==selection[t['mode']]['lambda'],'validation retuning or early inspection')
            cfg=read(Path(t['session'])/'config.effective.json');cfg.pop('baseline');configs.append(cfg)
        require(configs[0]==configs[1]==configs[2],'paired conditions differ')
    units=read(root/'verification/units_01/report.json');require(units['passed'],'unit tests failed')
    for f,h in units['source_hashes'].items():require(sha(repo/f)==h,'unit dependency changed '+f)
    for name in ('ablation_fault_tests','baseline_fault_tests'):require(read(root/'verification'/(name+'.json'))['passed'],'fault-injection audit failed')
    require(read(root/'verification/completed_rejection_tests.json')['passed'],'jitter guard witness tests failed')
    preflight=read(root/'verification/preflight_01/report.json');require(preflight['passed'] and len(preflight['cases'])==9,'short notification/mask cases incomplete')
    for name in ('decode_regression_01','state_regression_01','legacy_regression_01'):
        report=read(root/'verification'/name/'report.json');require(report['passed'],'regression failed '+name)
        if 'executable_sha256' in report:require(report['executable_sha256']==r['executable_sha256'],'regression binary changed')
        for c in report.get('cases',[]):require(read(Path(c['session']).parent.parent/'launch.json')['executable_sha256']==r['executable_sha256'],'regression case binary changed')
    total=dict(trials=42,training_trials=30,validation_trials=12,captures=sum(t['integration']['frames_joined'] for t in r['trials']),commands=sum(t['integration']['controller_commands_replayed'] for t in r['trials']),physics_steps=sum(t['integration']['applications_joined'] for t in r['trials']),candidates=sum(t['baseline']['candidate_rows'] for t in r['trials']),ip_bytes=sum(t['budget']['summary']['attempted_ip_bytes'] for t in r['trials']),pose_allowance_violations=sum(len(t['integration']['allowance_violations']) for t in r['trials']))
    total['validation']={mode:dict(outcomes=dict(Counter(t['integration']['motion']['outcome'] for t in validation if t['mode']==mode)),ip_bytes=sum(t['budget']['summary']['attempted_ip_bytes'] for t in validation if t['mode']==mode),counterfactual_changes=sum(t['ablation']['action_changed_against_same_input_B1'] for t in validation if t['mode']==mode),value_changed=sum(t['ablation']['value_changed'] for t in validation if t['mode']==mode)) for mode in plan['modes']}
    save(out/'aggregate.json',total)
    save(out/'task_decision.json',dict(passed=True,task='G3-02',completed_at=stamp(),all_completed=21,all_total=38,g3_completed=2,g3_total=5,gates_passed=3,gates_total=7,next='G3-03 small model and exact solution',aggregate=total,
        scope='initial information ablation implementation and equal-budget retuning; no statistical superiority or G3 gate',
        prior_invalid_evidence=['../../../reach_g3_ablation_20260923/verification/study_01/abort.json','../../../reach_g3_ablation_20260923/verification/study_02/report.json','../../../reach_g3_ablation_20260923/verification/preflight_01/report.json','../../../reach_g3_ablation_v2_20260923/verification/study_01/report.json','../../../reach_g3_ablation_v3_20260923/verification/study_01/report.json','../../../reach_g3_ablation_v4_20260923/verification/preflight_01/report.json'],prior_priority_check='../../../reach_g3_ablation_20260923/verification/priority_check_01_audit/report.json'))
    saved=out/'audit_source';saved.mkdir(exist_ok=True)
    for f in (repo/'tools').glob('*reach*.py'):shutil.copy2(f,saved/f.name)
    evidence={str(f.relative_to(repo)).replace('\\','/'):sha(f) for f in saved.glob('*.py')}
    for f in ('plan.json','baseline_lock.json','selection.json','report.json','aggregate.json','task_decision.json','index.html'):evidence[str((out/f).relative_to(repo)).replace('\\','/')]=sha(out/f)
    save(out/'evidence_audit.json',dict(passed=True,files=evidence,executable_sha256=r['executable_sha256']))
    print(json.dumps(total,indent=2))

if __name__=='__main__':main()
