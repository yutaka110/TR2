"""Serial real-UDP acceptance and independent audit of G4-03 scheduling decisions."""
import argparse,copy,csv,hashlib,json,math,subprocess,sys
from collections import Counter,defaultdict
from pathlib import Path
from unittest.mock import patch
from verify_reach_closed_loop import read,rows,save
from verify_reach_command import require
from verify_reach_actions import action_audit
from verify_reach_budget import budget_audit
from verify_reach_decode import decode_audit
from verify_reach_state import state_audit
from verify_reach_integration import integration_audit,direction
from verify_reach_prediction import independent

def percentile(xs,p):
    return sorted(xs)[max(0,math.ceil(len(xs)*p)-1)]

def recovery(cs):
    legal=[c for c in cs[1:] if c['eligible']=='1']
    repairs=[c for c in legal if c['kind']=='REPAIR']
    if repairs:return min(repairs,key=lambda c:int(c['capture_us']))['candidate']
    refresh=[c for c in legal if c['kind']=='REFRESH']
    if refresh:return refresh[-1]['candidate']
    fresh=[c for c in legal if int(c['local']) in (1,3)]
    return max(fresh,key=lambda c:(int(c['capture_us']),c['local']=='3'))['candidate'] if fresh else '0'

def scheduler_audit(session):
    options=read(session/'ablation_model.json') if (session/'ablation_model.json').exists() else dict(variant='joint',**{'lambda':.1})
    variant=options['variant'];decode=variant in ('decode','joint');task=variant in ('task','joint');weight=options['lambda']
    ticks=rows(session/'scheduler.csv');candidates=rows(session/'scheduler_candidates.csv');executions=rows(session/'scheduler_execution.csv')
    require(ticks,'no scheduler ticks');groups=defaultdict(list)
    for c in candidates:groups[c['tick']].append(c)
    encoded={r['frame_id']:r for r in rows(session/'encoded.csv')}
    estimates={r['estimate_id']:r for r in rows(session/'sender_estimates.csv')}
    accepted={r['accepted_sequence']:r for r in rows(session/'state_rx.csv') if r['status']=='accepted'}
    feedback=rows(session/'prediction_feedback.csv');received=rows(session/'feedback_rx.csv')
    require(len(feedback)==len(received),'feedback count');loss=burst=.01;last=0;lastmissing=False
    for i,(f,r) in enumerate(zip(feedback,received),1):
        require(f['wire_hex']==r['wire_hex'] and int(f['id'])==i and int(f['event_us'])>=int(r['received_us']),'feedback provenance')
        b=bytes.fromhex(f['wire_hex'])
        if b[5]==5:
            base=int.from_bytes(b[44:48],'big');count=int.from_bytes(b[48:50],'big')
            require(len(b)==60+count*8,'feedback size')
            for at in range(60,len(b),8):
                seq=base+int.from_bytes(b[at:at+2],'big');flags=b[at+2]
                if seq<=last or not flags&3:continue
                missing=bool(flags&2);loss=.95*loss+.05*missing;burst=.95*burst+.05*(missing and lastmissing);lastmissing=missing;last=seq
        require(abs(float(f['loss'])-loss)<1e-10 and abs(float(f['burst'])-burst)<1e-10,'feedback estimate')
    origin=read(session/'ip_budget_summary.json')['origin_us'];duration=round(read(session/'config.effective.json')['duration_s']*1e6)
    decisions={r['decision_id']:r for r in rows(session/'action_candidates.csv') if r['selected']=='1'}
    previous=0
    for t in ticks:
        tick=int(t['tick']);now=int(t['snapshot_us']);start=int(t['start_us']);end=int(t['selection_end_us'])
        require(tick>previous and tick-previous-1==int(t['skipped']),'unrecorded skipped decision')
        require(int(t['scheduled_us'])==origin+tick*20000<=start<=now<=end<=int(t['dispatch_end_us']),'scheduler time/order')
        require(start<origin+duration and int(t['selection_us'])==end-start and int(t['dispatch_us'])==int(t['dispatch_end_us'])-end,'scheduler accounting')
        previous=tick;cs=groups[t['tick']];require(len(cs)==int(t['candidates'])<=22 and [int(c['candidate']) for c in cs]==list(range(len(cs))),'bounded candidate inventory')
        require(cs[0]['kind']=='DEFER' and cs[0]['eligible']=='1','no safe defer')
        require(all(int(t[k])<=now for k in ('sampled_us','received_us','generated_us','feedback_us')),'future scheduler input')
        if t['estimate_id']!='0':
            e=estimates[t['estimate_id']]
            for key,source in [('sampled_us','event_us'),('received_us','received_us'),('generated_us','generated_us'),('notification_sequence','accepted_sequence')]:require(t[key]==e[source],'estimate identity')
        b=bytes(208)
        if t['notification_sequence']!='0':
            rx=accepted[t['notification_sequence']];b=bytes.fromhex(rx['wire_hex']);require(rx['received_us']==t['received_us'],'unreceived state')
        get=lambda at,n:int.from_bytes(b[at:at+n],'big')
        f=feedback[int(t['feedback_id'])-1] if int(t['feedback_id']) else None
        if f:require(f['event_us']==t['feedback_us'],'feedback snapshot identity')
        for c in cs:
            require(c['causal']=='1','noncausal snapshot');frame=encoded.get(c['frame_id']);size=int(c['bytes']);cap=int(c['capture_us']);ip=int(c['ip_bytes']);local=int(c['local'])
            if size:require(frame and int(frame['bytes'])+56==size and frame['capture_us']==c['capture_us'] and int(frame['encoder_output_us'])<=now,'future/materialized frame')
            live=get(24,8)>0 and now-get(32,8)<250000 and (c['stream']=='0' or int(c['stream'])==get(48,4))
            reference=4 if get(56,4)<int(t['last_idr']) else get(72,2);stage=get(74,2)
            require(c['live']==str(int(live)),'stale state used');require(int(c['reference'])==(reference if decode else -1) and int(c['stage'])==(stage if task else -1),'hidden receiver state')
            missing=[int(x) for x in c['missing'].split(';') if x];rate=max(1,int(t['rate_bps']))
            expected=[(now-cap)/1000 if size else 0,ip/1000,int(t['queued_bytes'])*8000/rate,ip*8000/rate,float(f['loss']) if f else .01,float(f['burst']) if f else .01,
                (now-int(t['feedback_us']))/1000 if f else 1000,(now-get(32,8))/1000 if live else 1000,(now-get(104,8))/1000 if get(104,8) else 1000,max(0,get(40,8)-now)/1e6,
                int.from_bytes(b[128:132],'big',signed=True)/1e6,int.from_bytes(b[132:136],'big',signed=True)/1e6,int.from_bytes(b[136:140],'big',signed=True)/1e6,get(140,4)/1e6,get(88,8)/1000,len(missing)]
            value=1.
            if variant=='scalar' and live:
                dependency=1. if reference==1 else 2. if frame and frame['idr']=='1' else .15
                de=dependency*(1-.5*max(0,min(1,expected[14]/200)))
                st=2. if stage in (0,3) else 1. if stage==1 else 1.5
                tv=min(4.,st+max(0,min(1,expected[8]/200))+max(0,min(1,(expected[13]+.0003*expected[8])/.1))+(.5 if 0<expected[9]<=5 else 0))
                value=max(.075,min(4.,de*tv))
            require(abs(float(c.get('value','1'))-value)<1e-10,'scalar value provenance')
            if not decode:expected[14]=0
            if not task:
                for j in (8,10,11,12,13):expected[j]=0
            require(all(abs(float(c['x'+str(j)])-v)<1e-8 for j,v in enumerate(expected)),'scheduler feature mismatch')
            if local in (1,2,3,4,5) and c['eligible']=='1':
                require(0<=now-cap<200000 and c['generation']==c['active_generation'] and ip<=int(c['remaining_ip']),'ineligible candidate offered')
                require(c['offered']==('1' if local==5 else '0'),'repeated fresh / uncached repair')
            raw=float(c['raw_command'])
            if c['score']!='unknown' and local:
                require(raw>=0 and c['live']=='1' and int(c['support'])>=8 and int(c['runs'])>=2,'unsupported score')
                require(abs(float(c['score'])-(value*raw-weight*ip/65536))<1e-12,'calibrated or incorrect score used')
        selected=cs[int(t['selected'])];require(selected['eligible']=='1' and selected['frame_id']==t['frame_id'] and selected['kind']==t['kind'],'selection identity')
        if t['fallback']=='1':require(t['selected']==recovery(cs),'fallback policy mismatch')
        else:
            eligible=[c for c in cs if c['eligible']=='1' and c['kind'] not in ('DEFER','REFRESH')]
            require(eligible and all(float(c['raw_command'])>=0 for c in eligible),'partial support used')
            best=max([cs[0]]+eligible,key=lambda c:float(c['score']))
            require(t['selected']==best['candidate'] and t['reason']=='raw_short_horizon_proxy','not maximal proxy')
        if t['reason']=='compute_budget':require(t['fallback']=='1','timeout winner retained')
        if t['decision_id']!='0':
            d=decisions[t['decision_id']];require(int(d['event_us'])>=now and (d['kind']==t['kind'] if t['revalidation']=='eligible' else d['kind']=='DEFER'),'execution did not revalidate')
    require(set(groups)=={t['tick'] for t in ticks},'orphan candidates')
    bytick={t['tick']:t for t in ticks}
    for e in executions:
        t=bytick[e['tick']];require(int(e['event_us'])>=int(t['snapshot_us']),'execution before choice')
        if e['decision_id']!='0':require(e['decision_id'] in decisions and decisions[e['decision_id']]['kind']==e['kind'],'action authorization join')
        if e['status']=='issued':require(t['kind']=='REFRESH' and int(e['event_us'])-int(t['snapshot_us'])<200000,'unselected/expired refresh permit')
    times=[int(t['selection_us']) for t in ticks];dispatch=[int(t['dispatch_us']) for t in ticks]
    return dict(passed=True,ticks=len(ticks),candidate_rows=len(candidates),skipped=sum(int(t['skipped']) for t in ticks),
        reasons=dict(Counter(t['reason'] for t in ticks)),revalidation=dict(Counter(t['revalidation'] for t in ticks)),
        selection_p95_us=percentile(times,.95),selection_p99_us=percentile(times,.99),selection_max_us=max(times),selection_over_5ms=sum(v>5000 for v in times),
        dispatch_p99_us=percentile(dispatch,.99),dispatch_max_us=max(dispatch),start_lateness_p99_us=percentile([int(t['start_us'])-int(t['scheduled_us']) for t in ticks],.99),
        period_target_us=20000,hard_realtime_guarantee=False)

def kernel_audit(session,model):
    options=read(session/'ablation_model.json') if (session/'ablation_model.json').exists() else dict(variant='joint')
    variant=options['variant'];decode=variant in ('decode','joint');task=variant in ('task','joint')
    samples=[]
    with model.open() as f:
        next(f)
        for fields in csv.reader(f):
            v=[float(x) for x in fields];features=v[1:23]
            if not decode:features[3]=-1;features[6+14]=0
            if not task:
                features[4]=-1
                for j in (8,10,11,12,13):features[6+j]=0
            samples.append(dict(run=int(v[0]),features=features,y=v[23:29],delay=v[29:32]))
    cs=[c for c in rows(session/'scheduler_candidates.csv') if float(c['raw_command'])>=0]
    encoded={r['frame_id']:r for r in rows(session/'encoded.csv')};kinds={'FRESH':1,'PROTECT':2,'REPAIR':3,'REFRESH':4,'DEFER':0};checks=0
    for c in cs[::max(1,len(cs)//32)]:
        q=dict(features=[kinds[c['kind']],int(c['group']),int(encoded[c['frame_id']]['idr']),int(c['reference']),int(c['stage']),int(c['task'])]+[float(c['x'+str(j)]) for j in range(16)])
        expected,_=independent(samples,q);require(abs(expected[3]-float(c['raw_command']))<1e-12,'independent raw kernel mismatch');checks+=1
    return dict(passed=True,independent_queries=checks)

def fault_checks(session):
    variants=[];ts=rows(session/'scheduler.csv');cs=rows(session/'scheduler_candidates.csv')
    for field,value in [('scheduled_us','1'),('feedback_us',str(2**63)),('decision_id','9999999'),('selected','0')]:
        changed=copy.deepcopy(ts);row=next(t for t in changed if t['fallback']=='0' and t['selected']!='0');row[field]=value;variants.append((field,'scheduler.csv',changed))
    for field,value in [('score','987'),('x10','987')]:
        changed=copy.deepcopy(cs);row=next(c for c in changed if c['score']!='unknown' and c['kind']=='PROTECT');row[field]=value;variants.append((field,'scheduler_candidates.csv',changed))
    original=rows
    for name,file,content in variants:
        with patch(__name__+'.rows',side_effect=lambda p,file=file,content=content:content if p.name==file else original(p)):
            try:scheduler_audit(session)
            except (AssertionError,RuntimeError,ValueError,KeyError,IndexError):continue
        raise RuntimeError('audit accepted mutation '+name)
    return [v[0] for v in variants]

def main():
    p=argparse.ArgumentParser();p.add_argument('--name',required=True);p.add_argument('--build-name',default='reach_g4_scheduler_20260925');p.add_argument('--audit-session',type=Path);p.add_argument('--continue-audit',action='store_true',help='Preserve the failed report and reuse already recorded trials after an auditor correction');a=p.parse_args()
    if a.audit_session:print(json.dumps(scheduler_audit(a.audit_session)));return 0
    for v in (a.name,a.build_name):require(v.replace('_','').isalnum(),'unsafe name')
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/a.build_name;out=root/'verification'/a.name
    previous=None
    if a.continue_audit:
        previous=read(out/'report.json');require(not previous['passed'],'matrix already complete')
        backup=out/'report_before_auditor_fix.json';require(not backup.exists(),'already resumed');save(backup,previous)
    else:out.mkdir(parents=True,exist_ok=False)
    model=repo/'artifacts/reach_g4_prediction_20260924/verification/study_02/paths.csv';base=read(repo/'config/reach_rt_g2_budget.json');cases=[]
    for name in ('normal_T1','normal_T2','loss','budget','queue','pacer_backlog','blackout','timeout','no_model','p_missing'):
        duration=20 if name in ('normal_T1','normal_T2','loss') else 8;link=dict(uplink=direction(),downlink=direction());budget=copy.deepcopy(base)
        if name=='loss':link['uplink']['impairment']=[dict(at_us=t,delay_us=10000,drop=t%200000<12000) for t in range(0,duration*1000000,6000)]
        if name=='budget':budget['uplink']['max_ip_bytes']=45000
        if name=='queue':link['uplink']['capacity']=[dict(at_us=0,bps=200000)];link['uplink']['queue_ip_bytes']=6000
        if name=='pacer_backlog':budget['total']['rate_bps']=budget['uplink']['rate_bps']=100000
        if name=='blackout':link['downlink']['impairment']=[dict(at_us=0,delay_us=10000,drop=False),dict(at_us=2500000,delay_us=0,drop=True)]
        cases.append(dict(name=name,duration_s=duration,link=link,budget=budget))
    save(out/'plan.json',dict(cases=cases,retries=0,model_sha256=hashlib.sha256(model.read_bytes()).hexdigest(),scope='G4-03 plumbing/timing/fallback acceptance; not G5 or effectiveness ranking'))
    report=dict(passed=False,cases=[])
    try:
        for case in cases:
            prior=next((c for c in previous['cases'] if c['name']==case['name']),None) if previous else None
            if prior and prior['passed']:report['cases'].append(prior);continue
            name=case['name'];save(out/(name+'_link.json'),case['link']);save(out/(name+'_budget.json'),case['budget'])
            cmd=[sys.executable,'tools/run_reach_g1.py','--name',a.name+'_'+name,'--build-name',a.build_name,'--stage','command_udp','--task','T1' if name=='normal_T1' else 'T2','--duration',str(case['duration_s']),'--baseline','G4-03','--state-feedback','--packet-trace','--process-priority','above_normal','--precise-wait','--budget-config',str(out/(name+'_budget.json')),'--link-config',str(out/(name+'_link.json'))]
            if name!='no_model':cmd+=['--prediction-model',str(model)]
            if name=='timeout':cmd+=['--scheduler-diagnostic','timeout']
            if name=='p_missing':cmd+=['--diagnostic-drop-frame','30']
            if prior:
                launch=read(root/'runs'/(a.name+'_'+name)/'launch.json');require(launch['exit_code']==0,'cannot reuse failed native trial');code=0
            else:
                r=subprocess.run(cmd,cwd=repo,capture_output=True);(out/(name+'.stdout')).write_bytes(r.stdout);(out/(name+'.stderr')).write_bytes(r.stderr);code=r.returncode
            session=next((root/'runs'/(a.name+'_'+name)/'sessions').iterdir());entry=dict(name=name,session=str(session),passed=False);report['cases'].append(entry)
            require(code==0,'native trial failed '+name)
            entry.update(scheduler=scheduler_audit(session),action=action_audit(session),budget=budget_audit(session),decode=decode_audit(session),state=state_audit(session),integration=integration_audit(session,out/name))
            require(not entry['integration']['allowance_violations'],'pose allowance exceeded')
            sched=entry['scheduler'];act=entry['action']
            require(sched['selection_p95_us']<=2000 and sched['selection_p99_us']<=5000,'scheduler p95/p99 target failed')
            if name.startswith('normal'):
                require(sched['reasons'].get('raw_short_horizon_proxy',0)>0,'prediction selection not exercised');entry['kernel']=kernel_audit(session,model)
                require(act['xor_packets_checked']>0 and act['idr_outputs']>0,'no real FEC / refresh')
            if name=='normal_T2':entry['mutation_checks']=fault_checks(session)
            if name=='loss':require(act['sent_classes'].get('retransmission',0)>0,'delayed real repair not exercised')
            if name=='budget':require(any(c['eligibility']=='byte_budget_insufficient' for c in rows(session/'scheduler_candidates.csv')),'budget never exhausted')
            if name=='queue':require(sched['reasons'].get('out_of_domain',0)>0,'OOD not exercised')
            if name=='timeout':require(sched['reasons'].get('compute_budget',0)>0 and sched['reasons'].get('raw_short_horizon_proxy',0)==0,'synthetic timeout ineffective')
            if name=='no_model':require(sched['reasons'].get('calibration_not_loaded',0)>0,'missing model not handled')
            if name=='p_missing':require(next(r for r in rows(session/'encoded.csv') if r['frame_id']=='30')['idr']=='0','omitted frame was not P')
            if name=='blackout':
                world=rows(session/'world.csv');require(max(float(w['v_m_s']) for w in world)>.05,'no movement before blackout');require(all(float(w['v_m_s'])==0 for w in world if float(w['simulation_s'])>4),'no braking after blackout')
                require(sched['reasons'].get('notification_unavailable',0)>10,'stale notification fallback not exercised')
            entry['passed']=True;save(out/'report.json',report);print(json.dumps(dict(case=name,scheduler=sched)),flush=True)
        if previous:report['auditor_correction']='Budget exhaustion is checked in all scheduler candidates, including discarded alternatives; action_candidates contains the executed decision only. No native trial rerun or result omission.'
        report['passed']=True
    except Exception as e:report['error']=repr(e);print(repr(e),flush=True)
    save(out/'report.json',report);return 0 if report['passed'] else 1
if __name__=='__main__':sys.exit(main())
