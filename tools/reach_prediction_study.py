"""Predeclared whole-run development/calibration/validation of G4-02 forecasts.

Future logs enter labels ONLY, never live features or candidate generation.
Outcomes describe the fixed cyclic continuation, not isolated causal effects.
"""
import argparse,bisect,copy,csv,hashlib,json,math,subprocess,sys
from collections import Counter,defaultdict
from pathlib import Path
from verify_reach_closed_loop import read,rows,save
from verify_reach_command import require
from verify_reach_actions import action_audit
from verify_reach_budget import budget_audit
from verify_reach_decode import decode_audit
from verify_reach_state import state_audit
from verify_reach_integration import integration_audit,direction

SCALES=[60,12,50,50,.12,.08,150,100,150,20,1,.4,1,.15,60,8]
KINDS={'DEFER':0,'FRESH':1,'PROTECT':2,'REPAIR':3,'REFRESH':4}
NAMES=['arrival','decoded','reference_at_horizon','command_use','later_image_use','new_completion']
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def values(row):return [int(row[k]) for k in ('kind','group','idr','reference','stage','task')]+[float(row['x'+str(i)]) for i in range(16)]
def paths_write(path,samples):
    with path.open('w',newline='',encoding='utf-8') as f:
        f.write('reach_paths_v2\n');writer=csv.writer(f)
        for s in samples:writer.writerow([s['run']]+s['features']+s['y']+s['delay'])

def feature_audit(session):
    candidates={(r['decision_id'],str(i%7)):r for i,r in enumerate(rows(session/'action_candidates.csv'))}
    estimates={r['estimate_id']:r for r in rows(session/'sender_estimates.csv')}
    accepted={str(int.from_bytes(bytes.fromhex(r['wire_hex'])[24:32],'big')):r for r in rows(session/'state_rx.csv') if r['status']=='accepted'}
    fbs=rows(session/'prediction_feedback.csv');actual=rows(session/'feedback_rx.csv')
    require(len(fbs)==len(actual),'feedback not identical');loss=burst=.01;last=0;lastmissing=False
    for n,(f,a) in enumerate(zip(fbs,actual),1):
        require(f['wire_hex']==a['wire_hex'] and int(f['event_us'])>=int(a['received_us']) and int(f['id'])==n,'feedback causality')
        b=bytes.fromhex(f['wire_hex'])
        if b[5]==5:
            base=int.from_bytes(b[44:48],'big');count=int.from_bytes(b[48:50],'big')
            require(len(b)==60+count*8,'feedback entry length')
            for pos in range(60,60+count*8,8):
                seq=base+int.from_bytes(b[pos:pos+2],'big');flags=b[pos+2]
                if seq<=last or not flags&3:continue
                missing=bool(flags&2);loss=.95*loss+.05*missing;burst=.95*burst+.05*(missing and lastmissing);lastmissing=missing;last=seq
        # The protocol-specific feedback parser is additionally checked against C++ golden data below.
        require(abs(float(f['loss'])-loss)<1e-10 and abs(float(f['burst'])-burst)<1e-10,'feedback estimator mismatch')
    allrows=rows(session/'predictions.csv')
    require(len(allrows)==len(candidates),'missing prediction rows')
    for p in allrows:
        c=candidates[(p['decision_id'],p['candidate'])];now=int(p['event_us'])
        require(now==int(c['event_us']) and int(p['kind'])==KINDS[c['kind']] and p['group']==c['group'] and p['eligible']==c['eligible'],'candidate join mismatch')
        require(all(int(p[k])<=now for k in ('sampled_us','received_us','generated_us','feedback_us')) and p['causal']=='1','future input')
        if p['estimate_id']!='0':
            e=estimates[p['estimate_id']]
            for k,ek in [('sampled_us','event_us'),('received_us','received_us'),('generated_us','generated_us'),('notification_sequence','accepted_sequence'),('decoded_frame','decoded_frame')]:require(p[k]==e[ek],'state estimate provenance')
        if p['notification_sequence']!='0':
            rx=accepted[p['notification_sequence']];b=bytes.fromhex(rx['wire_hex'])
            require(int(rx['received_us'])==int(p['received_us']),'unreceived notification')
            ref=4 if int(p['decoded_frame'])<int(p['last_idr']) else int.from_bytes(b[72:74],'big')
            require(int(p['reference'])==ref and int(p['stage'])==int.from_bytes(b[74:76],'big'),'state field leaked')
            require(int(p['report_stream'])==int.from_bytes(b[48:52],'big'),'report stream mismatch')
        else:b=bytes(208)
        live=int(p['notification_sequence'])>0 and now-int(p['generated_us'])<250000 and (c['stream_id']=='0' or p['report_stream']==c['stream_id'])
        require(p['live']==str(int(live)),'stale notification consumed')
        rate=max(1,int(p['rate_bps']));require(abs(float(p['x2'])-int(p['queued_bytes'])*8000/rate)<1e-8 and abs(float(p['x3'])-int(c['ip_bytes'])*8000/rate)<1e-8,'queue/serialization feature')
        get=lambda at,n:int.from_bytes(b[at:at+n],'big')
        cap=get(104,8);deadline=get(40,8)
        expected={0:(now-int(c['capture_us']))/1000 if int(c['payload_bytes']) else 0,1:int(c['ip_bytes'])/1000,6:(now-int(p['feedback_us']))/1000 if int(p['feedback_us']) else 1000,
          7:(now-int(p['generated_us']))/1000 if live else 1000,8:(now-cap)/1000 if cap else 1000,9:max(0,deadline-now)/1e6,
          10:int.from_bytes(b[128:132],'big',signed=True)/1e6,11:int.from_bytes(b[132:136],'big',signed=True)/1e6,12:int.from_bytes(b[136:140],'big',signed=True)/1e6,13:get(140,4)/1e6,14:get(88,8)/1000,15:int(c['missing_count'])}
        for j,v in expected.items():require(abs(float(p['x'+str(j)])-v)<1e-8,'feature mismatch '+str(j))
        if int(p['feedback_id']):
            f=fbs[int(p['feedback_id'])-1];require(p['feedback_us']==f['event_us'] and float(p['x4'])==float(f['loss']) and float(p['x5'])==float(f['burst']),'network feature provenance')
        else:require(p['feedback_us']=='0' and float(p['x4'])==.01 and float(p['x5'])==.01,'initial network prior mismatch')
    return dict(passed=True,rows=len(allrows),feedback=len(fbs),statuses=dict(Counter(r['status'] for r in allrows)))

def dataset(session,run_id):
    selected={r['decision_id']:r for r in rows(session/'action_candidates.csv') if r['selected']=='1'}
    preds=rows(session/'predictions.csv');encoded={int(r['frame_id']):r for r in rows(session/'encoded.csv')}
    completed=defaultdict(list);decoded=defaultdict(list)
    for r in rows(session/'reassembly_events.csv'):
        if r['outcome']=='completed':completed[int(r['frame_id'])].append(int(r['event_us']))
    for r in rows(session/'decoded_audit.csv'):
        if r['identity_match']=='1':decoded[int(r['frame_id'])].append(int(r['decoded_us']))
    requested={int(r['related_frame']):int(r['frame_id']) for r in rows(session/'action_events.csv') if r['event']=='idr_output'}
    applied=rows(session/'udp_applied_commands.csv');times=[int(r['applied_us']) for r in applied]
    world={int(r['physics_tick']):r for r in rows(session/'world.csv')}
    states=rows(session/'state_tx.csv');stimes=[int(r['generated_us']) for r in states]
    origin=read(session/'ip_budget_summary.json')['origin_us'];duration=read(session/'config.effective.json')['duration_s'];end=origin+round(duration*1e6)
    result=[];excluded=Counter()
    for p in preds:
        c=selected[p['decision_id']]
        if KINDS[c['kind']]!=int(p['kind']) or c['group']!=p['group']:continue
        now=int(p['event_us']);horizon=now+200000
        if horizon>end:excluded['right_censored']+=1;continue
        if p['live']!='1' or p['causal']!='1':excluded['notification_unavailable']+=1;continue
        fid=int(p['frame_id']);kind=int(p['kind'])
        if kind==4:fid=requested.get(fid,0)
        if kind==0:fid=0
        limit=min(horizon,int(encoded[fid]['capture_us'])+200000) if fid in encoded else horizon
        # Already available at the decision counts as ready with zero remaining wait.
        def first(events):return min((max(now,t) for t in events if t<=limit),default=None)
        arrival=first(completed[fid]) if fid else None;decode=first(decoded[fid]) if fid else None
        lo=bisect.bisect_left(times,now);hi=bisect.bisect_right(times,horizon);future=applied[lo:hi]
        command=min((int(r['applied_us']) for r in future if fid and int(r['source_frame_id'])==fid and r['live']=='1' and int(r['applied_us'])<=limit),default=None)
        # Future images are an observed continuation outcome, never credited as an isolated action effect.
        later=any(r['live']=='1' and int(r['source_frame_id'])>max(fid,int(p['decoded_frame'])) for r in future)
        stateindex=bisect.bisect_right(stimes,horizon)-1
        if stateindex<0 or horizon-stimes[stateindex]>100000:excluded['missing_horizon_label']+=1;continue
        b=bytes.fromhex(states[stateindex]['wire_hex']);reference=int.from_bytes(b[72:74],'big')==1
        before=world[int(applied[max(0,lo-1)]['physics_tick'])]['success']=='1'
        finish=not before and any(world[int(r['physics_tick'])]['success']=='1' for r in future)
        result.append(dict(run=run_id,session=str(session),decision_id=p['decision_id'],features=values(p),y=[int(arrival is not None),int(decode is not None),int(reference),int(command is not None),int(later),int(finish)],delay=[(t-now)/1000 if t is not None else 999 for t in (arrival,decode,command)],native=p))
    return result,dict(excluded)

def probe(exe,model,query):
    r=subprocess.run([str(exe),str(model),str(query)],capture_output=True,check=True)
    output=[]
    for line in r.stdout.decode().splitlines():
        f=line.split(',');output.append(dict(status=f[1],support=int(f[2]),runs=int(f[3]),raw=[float(f[6+2*j]) for j in range(6)],p=[float(f[7+2*j]) for j in range(6)],delays=[float(x) for x in f[18:24]]))
    return output

def metrics(samples,predictions):
    result={};support=sum(p['raw'][0]>=0 for p in predictions)
    for j,name in enumerate(NAMES):
        pairs=[(s['y'][j],p['raw'][j],p['p'][j]) for s,p in zip(samples,predictions) if p['raw'][j]>=0]
        calibrated=[(y,p) for y,r,p in pairs if p>=0]
        bins=[]
        for b in range(5):
            selected=[(y,p) for y,p in calibrated if min(4,int(p*5))==b]
            if selected:bins.append(dict(bin=b,n=len(selected),predicted=sum(p for y,p in selected)/len(selected),observed=sum(y for y,p in selected)/len(selected)))
        result[name]=dict(n=len(pairs),positive=sum(y for y,r,p in pairs),raw_brier=sum((r-y)**2 for y,r,p in pairs)/len(pairs) if pairs else None,
          calibrated_n=len(calibrated),calibrated_brier=sum((p-y)**2 for y,p in calibrated)/len(calibrated) if calibrated else None,reliability=bins)
    return dict(rows=len(samples),supported=support,out_of_domain=len(samples)-support,outcomes=result)

def main():
    p=argparse.ArgumentParser();p.add_argument('--name',required=True);p.add_argument('--build-name',default='reach_g4_prediction_20260924');p.add_argument('--build',default='build_03');p.add_argument('--reuse-development',type=Path);a=p.parse_args()
    for v in (a.name,a.build_name,a.build):require(v.replace('_','').isalnum(),'unsafe name')
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/a.build_name;out=root/'verification'/a.name;out.mkdir(parents=True,exist_ok=False)
    exe=root/'verification'/a.build/'prediction_bin/reach_prediction_probe.exe';base=read(repo/'config/reach_rt_g2_budget.json')
    plan=[]
    for split,repeats in [('train',2),('calibration',1),('validation',1)]:
        for repeat in range(repeats):
            for task in ('T1','T2'):
                for condition in ('normal','burst'):plan.append(dict(split=split,task=task,condition=condition,repeat=repeat))
    plan += [dict(split='stress',task='T2',condition=c,repeat=0) for c in ('queue','blackout')]
    save(out/'plan.json',dict(cases=plan,duration_s=20,retries=0,split_unit='whole sequential run',horizon_us=200000,calibration='five fixed raw probability bins; >=16 samples from >=2 calibration runs; empirical bin frequency',minimum_support='8 neighbors and 2 training runs',policy='deterministic cyclic four media actions; shared received repairs/refresh',validation_is_G5=False))
    report=dict(passed=False,cases=[]);data=defaultdict(list);model=out/'paths.csv'
    try:
        if a.reuse_development:
            historical=read(a.reuse_development/'report.json');require(historical['passed'],'development collection not complete')
            for i,entry in enumerate(historical['cases'][:12],1):
                entry=copy.deepcopy(entry);session=Path(entry['session']);require(entry['split'] in ('train','calibration') and entry['passed'],'invalid reuse split')
                entry['features']=feature_audit(session);xs,excluded=dataset(session,i);data[entry['split']]+=xs;entry.update(samples=len(xs),excluded=excluded,reused_collection=True);report['cases'].append(entry)
            save(out/'relabel_provenance.json',dict(source=str(a.reuse_development),source_report_sha256=sha(a.reuse_development/'report.json'),used='first eight train and four calibration runs ONLY',reason='v2 readiness includes already reassembled/decoded at decision; structural probability projection',new_validation=True))
        for i,case in enumerate(plan,1):
            if a.reuse_development and i<=12:continue
            split=case['split'];name=f'{i:02}_{split}_{case["task"]}_{case["condition"]}';link=dict(uplink=direction(),downlink=direction());budget=copy.deepcopy(base)
            if case['condition']=='burst':link['uplink']['impairment']=[dict(at_us=t,delay_us=10000+(i%3)*2000,drop=((t+i*6000)%240000)<18000) for t in range(0,20000000,6000)]
            if case['condition']=='queue':link['uplink']['capacity']=[dict(at_us=0,bps=100000)];link['uplink']['queue_ip_bytes']=5000
            if case['condition']=='blackout':link['downlink']['impairment']=[dict(at_us=0,delay_us=10000,drop=False),dict(at_us=3000000,delay_us=0,drop=True)]
            save(out/(name+'_link.json'),link);save(out/(name+'_budget.json'),budget)
            if split!='train' and not model.exists():
                paths_write(model,data['train']);save(out/'fit_lock.json',dict(training_runs=[c['session'] for c in report['cases'] if c['split']=='train'],samples=len(data['train']),sha256=sha(model)))
            if split in ('validation','stress') and not Path(str(model)+'.cal').exists():
                query=out/'calibration_queries.csv';paths_write(query,data['calibration']);raw=probe(exe,model,query);table=[];cells=[]
                for j in range(6):
                    for b in range(5):
                        xs=[(s,p) for s,p in zip(data['calibration'],raw) if p['raw'][j]>=0 and min(4,int(p['raw'][j]*5))==b];runs={s['run'] for s,p in xs}
                        value=sum(s['y'][j] for s,p in xs)/len(xs) if len(xs)>=16 and len(runs)>=2 else -1
                        table.append(value);cells.append(dict(outcome=NAMES[j],bin=b,n=len(xs),runs=len(runs),value=value))
                Path(str(model)+'.cal').write_text(' '.join(map(str,table)),encoding='utf-8');save(out/'calibration_lock.json',dict(cells=cells,sha256=sha(Path(str(model)+'.cal')),runs=[c['session'] for c in report['cases'] if c['split']=='calibration']))
            cmd=[sys.executable,'tools/run_reach_g1.py','--name',a.name+'_'+name,'--build-name',a.build_name,'--stage','command_udp','--task',case['task'],'--duration','20','--baseline','G4-02','--state-feedback','--packet-trace','--process-priority','above_normal','--precise-wait','--budget-config',str(out/(name+'_budget.json')),'--link-config',str(out/(name+'_link.json'))]
            if split!='train':cmd+=['--prediction-model',str(model)]
            r=subprocess.run(cmd,cwd=repo,capture_output=True);(out/(name+'.stdout')).write_bytes(r.stdout);(out/(name+'.stderr')).write_bytes(r.stderr)
            session=next((root/'runs'/(a.name+'_'+name)/'sessions').iterdir());entry=dict(name=name,split=split,session=str(session),passed=False);report['cases'].append(entry)
            require(r.returncode==0,'native trial failed '+name)
            entry.update(features=feature_audit(session),action=action_audit(session),budget=budget_audit(session),decode=decode_audit(session),state=state_audit(session),integration=integration_audit(session,out/name))
            require(not entry['integration']['allowance_violations'],'pose allowance exceeded')
            samples,excluded=dataset(session,i);data[split]+=samples;entry.update(samples=len(samples),excluded=excluded,passed=True)
            save(out/'report.json',report);print(json.dumps(dict(case=name,rows=len(samples),features=entry['features'])),flush=True)
        for split,samples in data.items():
            path=out/(split+'_queries.csv');paths_write(path,samples);predictions=probe(exe,model,path);save(out/(split+'_metrics.json'),metrics(samples,predictions))
            save(out/(split+'_labels.json'),[{k:v for k,v in s.items() if k!='native'} for s in samples]);save(out/(split+'_predictions.json'),predictions)
        report['passed']=True
    except Exception as e:report['error']=repr(e);print(repr(e),flush=True)
    save(out/'report.json',report);return 0 if report['passed'] else 1
if __name__=='__main__':sys.exit(main())
