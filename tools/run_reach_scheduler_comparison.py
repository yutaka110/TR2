"""G4-04 predeclared serial tuning and condition-wise live comparison. No retries."""
import argparse,copy,hashlib,json,subprocess,sys
from datetime import datetime
from pathlib import Path
from verify_reach_closed_loop import read,rows,save
from verify_reach_command import require
from verify_reach_scheduler import scheduler_audit,kernel_audit
from verify_reach_actions import action_audit
from verify_reach_baseline import baseline_audit
from verify_reach_budget import budget_audit
from verify_reach_decode import decode_audit
from verify_reach_state import state_audit
from verify_reach_integration import integration_audit,direction
from generate_reach_trace import write_bundle
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def stamp():return datetime.now().astimezone().isoformat()
VARIANTS=('common','decode','task','joint','scalar')
TUNED=VARIANTS+('B1','B2','B3')
def main():
    p=argparse.ArgumentParser();p.add_argument('--name',default='study_01');p.add_argument('--build-name',default='reach_g4_comparison_20260925');p.add_argument('--build',default='build_01');a=p.parse_args()
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/a.build_name;out=root/'verification'/a.name;out.mkdir(parents=True,exist_ok=False)
    build=read(root/'verification'/a.build/'report.json');require(build['passed'],'no build');exe=root/'bin/Release/GE3.exe';model=repo/'artifacts/reach_g4_prediction_20260924/verification/study_02/paths.csv'
    sources=build['source_hashes'];critical={f:sha(repo/f) for f in ('tools/run_reach_scheduler_comparison.py','tools/run_reach_g1.py','tools/verify_reach_scheduler.py')}
    require(all(sha(repo/f)==h for f,h in sources.items()),'build/source mismatch')
    base=read(repo/'config/reach_rt_g2_budget.json');cases=[]
    for name,task,phase,duration in [('tune_T1','T1','tuning',20),('tune_T2','T2','tuning',20),('normal_T1','T1','validation',20),('normal_T2','T2','validation',20),('idr_missing','T2','validation',20),('p_missing','T2','validation',20),('mixed_small_queue','T2','validation',20),('recognition_stall','T2','validation',20),('notification_reorder','T2','validation',8),('notification_blackout','T2','validation',8),('unknown_capacity','T2','validation',8),('byte_budget','T2','validation',8),('pacer_backlog','T2','validation',8),('compute_cutoff','T2','validation',8)]:
        c=dict(name=name,task=task,phase=phase,duration_s=duration,link=dict(uplink=direction(),downlink=direction()),budget=copy.deepcopy(base),diagnostics=[])
        if name in ('tune_T1','tune_T2','mixed_small_queue'):
            seed={'tune_T1':901,'tune_T2':902,'mixed_small_queue':943}[name];q=65536 if name=='tune_T1' else 16384 if name=='tune_T2' else 8192
            profile={d:dict(queue_ip_bytes=q if d=='uplink' else 16384,capacities_bps=[2500000,6000000] if name=='tune_T1' else [1500000,6000000],drop_ppm=10000 if name=='tune_T1' else 20000,base_delay_us=10000,jitter_us=10000) for d in ('uplink','downlink')}
            c['link'],meta=write_bundle(out/(name+'_trace'),seed=seed,horizon_us=duration*1000000,slot_us=100000,profile=profile);c['trace_sha256']=meta['link_sha256'];c['seed']=seed
        if name=='idr_missing':c['diagnostics']=['--diagnostic-drop-frame','1']
        if name=='p_missing':c['diagnostics']=['--diagnostic-drop-frame','30']
        if name=='recognition_stall':c['diagnostics']=['--diagnostic-recognition-delay-ms','250']
        if name=='notification_reorder':c['link']['downlink']['impairment']=[dict(at_us=t,delay_us=180000 if (t//100000)%4==0 else 10000,drop=False) for t in range(0,duration*1000000,100000)]
        if name=='notification_blackout':c['link']['downlink']['impairment']=[dict(at_us=0,delay_us=10000,drop=False),dict(at_us=2500000,delay_us=0,drop=True)]
        if name=='unknown_capacity':c['link']['uplink']['capacity']=[dict(at_us=0,bps=200000)];c['link']['uplink']['queue_ip_bytes']=6000
        if name=='byte_budget':c['budget']['uplink']['max_ip_bytes']=45000
        if name=='pacer_backlog':c['budget']['total']['rate_bps']=c['budget']['uplink']['rate_bps']=100000
        if name=='compute_cutoff':c['diagnostics']=['--scheduler-diagnostic','timeout']
        save(out/(name+'_link.json'),c['link']);save(out/(name+'_budget.json'),c['budget']);cases.append(c)
    plan=dict(created=stamp(),cases=cases,tuned_modes=TUNED,lambdas=[.1,1.],tuning_trials=32,validation_trials=72,
        selection='successes descending; terminal seconds with failure=20; attempted IP bytes; lambda list order',
        fixed_coefficient_comparison='separate native replay at lambda=.1, same recorded snapshot/candidates; not alternate closed-loop outcomes',
        validation_order='rotate five masks by condition; B0/B1/B2/B3 also on both normal tasks and mixed-small-queue; tuned parameters frozen first',
        invalid_policy='no retries; preserve invalid; stop tuning before selection; validation invalid prevents completion',
        scope='exploratory development, one trial per cell; 20s task comparison and 8s diagnostics; not 60s/G5/confidence claim',
        executable_sha256=sha(exe),model_sha256=sha(model),source_sha256=sources,critical_sha256=critical,calibration_not_used=True)
    save(out/'plan.json',plan);report=dict(passed=False,complete=False,trials=[])
    def run(c,mode,weight):
        tid=c['name']+'_'+mode+'_'+str(weight).replace('.','p');t=dict(id=tid,case=c['name'],mode=mode,**{'lambda':weight},phase=c['phase'],duration_s=c['duration_s'],passed=False,started=stamp());report['trials'].append(t);save(out/'report.json',report);print('START '+tid,flush=True)
        try:
            require(sha(exe)==plan['executable_sha256'] and all(sha(repo/f)==h for f,h in {**sources,**critical}.items()),'frozen implementation changed')
            cmd=[sys.executable,'tools/run_reach_g1.py','--name',a.name+'_'+tid,'--build-name',a.build_name,'--stage','command_udp','--task',c['task'],'--duration',str(c['duration_s']),'--baseline','G4-04' if mode in VARIANTS else mode,'--baseline-lambda',str(weight),'--state-feedback','--packet-trace','--process-priority','above_normal','--precise-wait','--budget-config',str(out/(c['name']+'_budget.json'))]
            cmd+=['--trace-bundle',str(out/(c['name']+'_trace'))] if 'seed' in c else ['--link-config',str(out/(c['name']+'_link.json'))]
            if mode in VARIANTS:cmd+=['--scheduler-variant',mode,'--prediction-model',str(model)]
            cmd+=c['diagnostics'];result=subprocess.run(cmd,cwd=repo,capture_output=True);(out/(tid+'.stdout')).write_bytes(result.stdout);(out/(tid+'.stderr')).write_bytes(result.stderr)
            inv=root/'runs'/(a.name+'_'+tid);session=next((inv/'sessions').iterdir());t['session']=str(session);require(result.returncode==0,'native trial failed')
            config=read(session/'config.effective.json');require(config==read(inv/'requested_config.json') and config['ip_budget']==c['budget'] and config['link_model']==c['link'],'condition mismatch')
            t.update(budget=budget_audit(session),decode=decode_audit(session),state=state_audit(session),integration=integration_audit(session,out/tid))
            require(not t['integration']['allowance_violations'],'pose allowance exceeded')
            if mode in VARIANTS:
                t.update(scheduler=scheduler_audit(session),action=action_audit(session))
                # A bounded sample is checked independently for every variant before choosing lambda.
                if c['name']=='tune_T1' and weight==.1:t['kernel']=kernel_audit(session,model)
            else:t['baseline']=baseline_audit(session)
            t['passed']=True
        except Exception as e:t['error']=repr(e)
        t['finished']=stamp();save(out/'report.json',report)
        print(json.dumps(dict(completed=tid,passed=t['passed'],error=t.get('error'),outcome=t.get('integration',{}).get('motion',{}).get('outcome'),p99=t.get('scheduler',{}).get('selection_p99_us'))),flush=True)
        return t['passed']
    weights=[.1,1.]
    for ci,c in enumerate(c for c in cases if c['phase']=='tuning'):
        for wi,w in enumerate(weights if ci==0 else list(reversed(weights))):
            offset=(ci+wi)%len(TUNED)
            for mode in TUNED[offset:]+TUNED[:offset]:
                if not run(c,mode,w):report['error']='invalid tuning; no selection';save(out/'report.json',report);return 1
    selection={}
    for mode in TUNED:
        scores=[]
        for w in weights:
            ts=[t for t in report['trials'] if t['mode']==mode and t['lambda']==w];successes=sum(t['integration']['motion']['outcome']=='success' for t in ts)
            seconds=sum(t['integration']['motion']['terminal']['simulation_s'] if t['integration']['motion']['outcome']=='success' else 20 for t in ts)
            ip=sum(t['budget']['summary']['attempted_ip_bytes'] for t in ts);scores.append(dict(**{'lambda':w},successes=successes,terminal_seconds=seconds,ip_bytes=ip))
        selection[mode]=dict(chosen=min(scores,key=lambda s:(-s['successes'],s['terminal_seconds'],s['ip_bytes'],weights.index(s['lambda']))),all_scores=scores)
    save(out/'selection.json',dict(frozen=stamp(),holdout_used=False,plan_sha256=sha(out/'plan.json'),selection=selection));report['selection']=selection;save(out/'report.json',report)
    for ci,c in enumerate(c for c in cases if c['phase']=='validation'):
        modes=VARIANTS+('B0','B1','B2','B3') if c['name'] in ('normal_T1','normal_T2','mixed_small_queue') else VARIANTS
        offset=ci%len(modes)
        for mode in modes[offset:]+modes[:offset]:run(c,mode,.1 if mode=='B0' else selection[mode]['chosen']['lambda'])
    report['complete']=len(report['trials'])==104;report['passed']=report['complete'] and all(t['passed'] for t in report['trials']);save(out/'report.json',report)
    print(json.dumps(dict(passed=report['passed'],trials=len(report['trials']))),flush=True);return 0 if report['passed'] else 1
if __name__=='__main__':sys.exit(main())
