"""Same-snapshot native replay, observed conditional prediction error, live contrasts."""
import argparse,bisect,json,subprocess
from collections import Counter,defaultdict
from pathlib import Path
from verify_reach_closed_loop import read,rows,save
from verify_reach_command import require
from run_reach_scheduler_comparison import VARIANTS
KINDS={'DEFER':0,'FRESH':1,'PROTECT':2,'REPAIR':3,'REFRESH':4}
def samples(session,count=24):
    ts=rows(session/'scheduler.csv');groups=defaultdict(list)
    for c in rows(session/'scheduler_candidates.csv'):groups[c['tick']].append(c)
    accepted={r['accepted_sequence']:bytes.fromhex(r['wire_hex']) for r in rows(session/'state_rx.csv') if r['status']=='accepted'}
    encoded={r['frame_id']:r for r in rows(session/'encoded.csv')}
    eligible=[t for t in ts if any(c['eligible']=='1' and c['kind'] in ('FRESH','PROTECT','REPAIR') for c in groups[t['tick']])]
    indices=sorted({round(i*(len(eligible)-1)/(count-1)) for i in range(min(count,len(eligible)))}) if eligible else []
    result=[]
    for i in indices:
        t=eligible[i];now=int(t['snapshot_us']);packet=accepted.get(t['notification_sequence'],bytes(208));get=lambda at,n:int.from_bytes(packet[at:at+n],'big')
        candidates=[]
        for c in groups[t['tick']]:
            q=[float(c['x'+str(j)]) for j in range(16)]
            q[8]=(now-get(104,8))/1000 if get(104,8) else 1000
            q[10:15]=[int.from_bytes(packet[128:132],'big',signed=True)/1e6,int.from_bytes(packet[132:136],'big',signed=True)/1e6,int.from_bytes(packet[136:140],'big',signed=True)/1e6,get(140,4)/1e6,get(88,8)/1000]
            ref=4 if get(56,4)<int(t['last_idr']) else get(72,2);stage=get(74,2);frame=encoded.get(c['frame_id'])
            fields=[int(c['local']),KINDS[c['kind']],int(c['group']),int(c['ip_bytes']),int(c['eligible']),int(c['frame_id']),int(c['capture_us']),int(frame['idr']) if frame else 0,ref,stage,int(c['task']),int(c['live']),int(c['causal'])]+q
            candidates.append(dict(wire=' '.join(str(v) for v in fields),identity=(c['kind'],c['frame_id'],c['group']),eligible=c['eligible']))
        result.append(dict(tick=t,candidates=candidates))
    return result
def prediction_error(session):
    ticks={r['tick']:r for r in rows(session/'scheduler.csv')};applied=rows(session/'udp_applied_commands.csv');byframe=defaultdict(list)
    for a in applied:
        if a['live']=='1':byframe[a['source_frame_id']].append(int(a['applied_us']))
    origin=read(session/'ip_budget_summary.json')['origin_us'];end=origin+round(read(session/'config.effective.json')['duration_s']*1e6);pairs=[];excluded=Counter()
    for c in rows(session/'scheduler_candidates.csv'):
        t=ticks[c['tick']]
        if c['candidate']!=t['selected'] or c['kind'] not in ('FRESH','PROTECT','REPAIR'):continue
        now=int(t['snapshot_us']);p=float(c['raw_command'])
        if p<0:excluded['no_prediction']+=1;continue
        if t['revalidation']!='eligible':excluded['revalidation_rejected']+=1;continue
        if now+200000>end:excluded['right_censored']+=1;continue
        times=byframe[c['frame_id']];limit=min(now+200000,int(c['capture_us'])+200000);index=bisect.bisect_left(times,now);y=int(index<len(times) and times[index]<=limit);pairs.append((p,y))
    return dict(n=len(pairs),positive=sum(y for p,y in pairs),raw_brier=sum((p-y)**2 for p,y in pairs)/len(pairs) if pairs else None,excluded=dict(excluded),
        bins=[dict(bin=j,n=len(v),prediction=sum(p for p,y in v)/len(v),observed=sum(y for p,y in v)/len(v)) for j in range(5) if (v:=[(p,y) for p,y in pairs if min(4,int(p*5))==j])],scope='selected supported materialized action under its own continuation; different cohorts across policies, no causal gain or calibration superiority claim')
def main():
    p=argparse.ArgumentParser();p.add_argument('--name',default='analysis_01');p.add_argument('--study',default='study_01');p.add_argument('--build-name',default='reach_g4_comparison_20260925');p.add_argument('--build',default='build_01');a=p.parse_args()
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/a.build_name;source=root/'verification'/a.study;report=read(source/'report.json');require(report['passed'],'incomplete study')
    out=root/'verification'/a.name;out.mkdir(parents=True,exist_ok=False);exe=root/'verification'/a.build/'comparison_bin/reach_comparison_probe.exe';model=repo/'artifacts/reach_g4_prediction_20260924/verification/study_02/paths.csv'
    inputs=[];provenance=[];snapshots=[];queries=[];errors={}
    for t in report['trials']:
        if t['phase']!='validation' or t['mode'] not in VARIANTS:continue
        session=Path(t['session']);errors[t['id']]=prediction_error(session)
        for s in samples(session):
            sid=len(snapshots);snapshots.append(dict(trial=t['id'],mode=t['mode'],case=t['case'],tick=s['tick'],identities=[c['identity'] for c in s['candidates']]))
            for mode in range(5):queries.append((sid,mode,.1,False,s))
            if t['lambda']!=.1:queries.append((sid,VARIANTS.index(t['mode']),t['lambda'],True,s))
    for i,(sid,mode,weight,reproduce,s) in enumerate(queries):
        inputs.append(f'{i} {mode} {weight} {len(s["candidates"])}');inputs.extend(c['wire'] for c in s['candidates']);provenance.append(dict(query=i,snapshot=sid,variant=VARIANTS[mode],**{'lambda':weight},reproduce=reproduce))
    payload='\n'.join(inputs)+'\n';(out/'native_queries.txt').write_text(payload,encoding='utf-8');save(out/'query_provenance.json',provenance)
    run=subprocess.run([str(exe),str(model)],input=payload,text=True,capture_output=True,check=True);(out/'native_results.jsonl').write_text(run.stdout,encoding='utf-8');results=[json.loads(s) for s in run.stdout.splitlines()];require(len(results)==len(queries),'native replay count')
    fixed=defaultdict(dict);live_matches=0;cutoff_excluded=0
    for p,r in zip(provenance,results):
        require(r['id']==p['query'],'replay order');snap=snapshots[p['snapshot']];t=snap['tick'];is_own=p['variant']==snap['mode'] and (p['reproduce'] or next(x['lambda'] for x in report['trials'] if x['id']==snap['trial'])==.1)
        if is_own:
            if t['reason']=='compute_budget':cutoff_excluded+=1
            else:require(r['selected']==int(t['selected']) and r['reason']==t['reason'] and r['fallback']==int(t['fallback']),'live/native no-cutoff replay changed');live_matches+=1
        if not p['reproduce']:fixed[p['snapshot']][p['variant']]=r
    comparisons={};pairs=[('decode','common'),('task','common'),('joint','decode'),('joint','task'),('joint','scalar'),('joint','common')]
    for case in sorted({s['case'] for s in snapshots}):
        comparisons[case]={}
        for left,right in pairs:
            subset=[(snap,fixed[i]) for i,snap in enumerate(snapshots) if snap['case']==case];mutual=[(s,f) for s,f in subset if not f[left]['fallback'] and not f[right]['fallback']]
            comparisons[case][left+' vs '+right]=dict(n=len(subset),different_action=sum(f[left]['selected']!=f[right]['selected'] for s,f in subset),mutual_supported=len(mutual),different_with_mutual_support=sum(f[left]['selected']!=f[right]['selected'] for s,f in mutual),
                scope='same candidate IDs/costs/eligibility; observed state only; no alternate trajectory simulation')
    live={}
    for t in report['trials']:
        if t['phase']!='validation':continue
        motion=t['integration']['motion'];s=t.get('scheduler',{});live.setdefault(t['case'],{})[t['mode']]=dict(outcome=motion['outcome'],success=motion['outcome']=='success',seconds=motion['terminal']['simulation_s'] if motion['outcome']=='success' else t['duration_s'],duration_s=t['duration_s'],**{'lambda':t['lambda']},ip_bytes=t['budget']['summary']['attempted_ip_bytes'],fallback_rate=sum(n for k,n in s.get('reasons',{}).items() if k!='raw_short_horizon_proxy')/s['ticks'] if s else None,selection_p95_us=s.get('selection_p95_us'),selection_p99_us=s.get('selection_p99_us'),prediction_error=errors.get(t['id']))
    contrasts=[]
    for case,modes in live.items():
        joint=modes['joint']
        for other in ('common','decode','task','scalar','B0','B1','B2','B3'):
            if other not in modes:continue
            base=modes[other]
            label='joint_success_only' if joint['success'] and not base['success'] else 'joint_failure_only' if base['success'] and not joint['success'] else 'both_success' if joint['success'] else 'both_failed_window'
            contrasts.append(dict(case=case,comparison='joint vs '+other,classification=label,seconds_delta=joint['seconds']-base['seconds'],ip_delta=joint['ip_bytes']-base['ip_bytes'],scope='one exploratory pair; not statistical significance'))
    save(out/'report.json',dict(passed=True,live=live,contrasts=contrasts,same_snapshot=comparisons,snapshots=len(snapshots),native_queries=len(results),live_replay_matches=live_matches,live_cutoff_excluded=cutoff_excluded,
        replay_timing_us=dict(maximum=max(r['elapsed_us'] for r in results),over_5ms=sum(r['elapsed_us']>5000 for r in results),scope='offline no-cutoff diagnostic; not measured live latency'),fixed_coefficient=.1,calibration_used=False))
    print(json.dumps(dict(passed=True,snapshots=len(snapshots),queries=len(results),live_matches=live_matches,contrast_classes=dict(Counter(c['classification'] for c in contrasts)))));return 0
if __name__=='__main__':raise SystemExit(main())
