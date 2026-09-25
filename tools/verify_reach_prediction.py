"""Independent kernel, causality, split, calibration and immutable evidence audit."""
import argparse,copy,hashlib,html,json,math,re,shutil,sys
from collections import Counter
from pathlib import Path
from unittest.mock import patch
import reach_prediction_study as study
from verify_reach_closed_loop import read,rows,save
from verify_reach_command import require

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def independent(samples,query):
    candidates=[]
    for i,s in enumerate(samples):
        if s['features'][:6]!=query['features'][:6]:continue
        ds=[abs(a-b)/scale for a,b,scale in zip(s['features'][6:],query['features'][6:],study.SCALES)]
        distance=sum(d*d for d in ds)
        if max(ds)<=2 and distance<=9:candidates.append((distance,i))
    chosen=sorted(candidates)[:64];runs={samples[i]['run'] for d,i in chosen}
    if len(chosen)<8 or len(runs)<2:return [-1]*6,[]
    means=[sum(samples[i]['y'][j] for d,i in chosen)/len(chosen) for j in range(6)]
    quantiles=[]
    for j in range(3):
        values=sorted(samples[i]['delay'][j] for d,i in chosen)
        quantiles += [values[math.ceil(q*len(chosen))-1] for q in (.5,.9)]
    return means,quantiles

def main():
    p=argparse.ArgumentParser();p.add_argument('--name',required=True);p.add_argument('--study',default='study_02');p.add_argument('--build-name',default='reach_g4_prediction_20260924');a=p.parse_args()
    for v in (a.name,a.study,a.build_name):require(v.replace('_','').isalnum(),'unsafe name')
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/a.build_name;source=root/'verification'/a.study;out=root/'verification'/a.name;out.mkdir(parents=True,exist_ok=False)
    inputs={}
    def record(path):
        path=path.resolve();require(path.is_relative_to(repo),'outside repo');inputs[path.relative_to(repo).as_posix()]=sha(path);return path
    def load(path):return read(record(path))
    report=load(source/'report.json');plan=load(source/'plan.json');require(report['passed'] and len(report['cases'])==18,'incomplete study')
    build=load(root/'verification/build_03/report.json');historical_build=load(root/'verification/build_02/report.json');require(build['passed'] and historical_build['passed'],'build failed')
    for f,h in build['source_hashes'].items():require(sha(record(repo/f))==h,'current native source changed')
    require(sha(record(root/'bin/Release/GE3.exe'))==build['executable_sha256'],'current executable changed')
    native_tests=load(root/'verification/build_03/prediction_tests.json');require(native_tests['passed'],'prediction native tests failed')
    regressions=load(root/'verification/regression_01/report.json');require(regressions['passed'],'regression failed')
    units=load(root/'verification/regression_units_01/report.json');require(units['passed'],'unit regression failed')
    unit_count=sum(int(re.search(r'PASS (\d+)',t['output'])[1]) for t in units['tests']);require(unit_count==2000,'unit count')
    actions=load(root/'verification/actions_regression_01/report.json');require(actions['passed'] and len(actions['cases'])==7,'G4-01 regression incomplete')
    legacy=load(root/'verification/legacy_regression_01/report.json');require(legacy['passed'] and legacy['executable_sha256']==build['executable_sha256'],'legacy regression failure')
    action_tests=load(root/'verification/build_03/command_1.txt');require(action_tests['passed'] and action_tests['checks']==684,'native action tests')
    samples={s:load(source/(s+'_labels.json')) for s in ('train','calibration','validation','stress')}
    predictions={s:load(source/(s+'_predictions.json')) for s in samples}
    model=record(source/'paths.csv');cal=record(Path(str(model)+'.cal'))
    require(sha(model)==load(source/'fit_lock.json')['sha256'],'fit model changed')
    lock=load(source/'calibration_lock.json');require(sha(cal)==lock['sha256'],'calibration changed')
    split_runs={split:{s['run'] for s in xs} for split,xs in samples.items()}
    require(all(not split_runs[a]&split_runs[b] for a in split_runs for b in split_runs if a!=b),'run leakage')
    require(len(split_runs['train'])==8 and len(split_runs['calibration'])==4 and len(split_runs['validation'])==4,'missing run split')
    # Recreate the fit file from training labels; no calibration or validation labels in the deployed path library.
    expected=out/'recreated_paths.csv';study.paths_write(expected,samples['train']);require(sha(expected)==sha(model),'training labels/model mismatch')
    table=[float(v) for v in cal.read_text().split()];require(len(table)==30,'calibration shape')
    for cell in lock['cells']:
        j=study.NAMES.index(cell['outcome']);b=cell['bin'];xs=[(s,p) for s,p in zip(samples['calibration'],predictions['calibration']) if p['raw'][j]>=0 and min(4,int(p['raw'][j]*5))==b]
        value=sum(s['y'][j] for s,p in xs)/len(xs) if len(xs)>=16 and len({s['run'] for s,p in xs})>=2 else -1
        require(value==table[j*5+b]==cell['value'],'calibration used other split or bin')
    checks=0
    for split in ('calibration','validation','stress'):
        xs=samples[split];ps=predictions[split];require(len(xs)==len(ps),'prediction count')
        for s,pr in zip(xs,ps):
            raw=pr['raw'];pred=[table[j*5+min(4,int(raw[j]*5))] if raw[j]>=0 else -1 for j in range(6)]
            if raw[0]>=0:
                if s['features'][0]==0:
                    for j in (0,1,3):pred[j]=0
                if pred[0]>=0 and pred[1]>=0:pred[1]=min(pred[1],pred[0])
                if pred[1]>=0 and pred[3]>=0:pred[3]=min(pred[3],pred[1])
            require(pred==pr['p'],'calibration projection mismatch')
        indices=sorted(set(range(0,len(xs),max(1,len(xs)//64)))|{i for i,p in enumerate(ps) if p['raw'][5]>0})[:160]
        for i in indices:
            expected,quantiles=independent(samples['train'],xs[i]);actual=ps[i]
            require(all(abs(x-y)<1e-12 for x,y in zip(expected,actual['raw'])),'C++ independent probability mismatch')
            if quantiles:require(quantiles==actual['delays'],'joint path quantiles mismatch')
            checks+=1
        require(study.metrics(xs,ps)==load(source/(split+'_metrics.json')),'metric aggregation mismatch')
    for index,c in enumerate(report['cases'],1):
        session=Path(c['session']);run=session.parent.parent;launch=load(run/'launch.json')
        expected_build=historical_build if c.get('reused_collection') else build
        require(launch['executable_sha256']==expected_build['executable_sha256'] and launch['exit_code']==0,'trial build/failure')
        sourcehash=load(run/'source_hashes.json')
        for f,h in expected_build['source_hashes'].items():require(sourcehash[f]==h and sha(record(run/'research_source'/f))==h,'native snapshot mismatch')
        audit=study.feature_audit(session);require(audit==c['features'],'causal re-audit changed')
        if c['split']!='train' and not c.get('reused_collection'):require(sha(record(run/'prediction_paths.csv'))==sha(model),'trial fit model mismatch')
        if c['split'] in ('validation','stress'):require(sha(record(run/'prediction_paths.csv.cal'))==sha(cal),'trial calibration mismatch')
        elif c['split']=='calibration':require(not (run/'prediction_paths.csv.cal').exists(),'calibration used future calibration parameters')
        for f in session.iterdir():
            if f.is_file():record(f)
        record(run/'requested_config.json')
        rebuilt,excluded=study.dataset(session,index);original=[s for s in samples[c['split']] if s['run']==index]
        require([{k:v for k,v in s.items() if k!='native'} for s in rebuilt]==original,'future-label reconstruction mismatch')
        if c['split'] in ('validation','stress'):
            recorded={s['decision_id']:s['native'] for s in rebuilt}
            for s,pr in zip(samples[c['split']],predictions[c['split']]):
                if s['run']!=index:continue
                native=recorded[s['decision_id']]
                require(all(abs(float(native['raw'+str(j)])-pr['raw'][j])<1e-12 and abs(float(native['p'+str(j)])-pr['p'][j])<1e-12 for j in range(6)),'live/replay prediction mismatch')
    # Mutate copies in memory: original trial evidence is immutable.
    session=Path(report['cases'][0]['session']);base=rows(session/'predictions.csv');original_rows=study.rows;faults=[]
    for field,value in [('received_us','999999999999999999'),('estimate_id','999999'),('x4','0.987654'),('stage','9'),('x10','99'),('live','0')]:
        modified=copy.deepcopy(base);next(r for r in modified if r['live']=='1')[field]=value
        with patch.object(study,'rows',side_effect=lambda p:modified if p.name=='predictions.csv' else original_rows(p)):
            rejected=False
            try:study.feature_audit(session)
            except (RuntimeError,AssertionError,KeyError,ValueError):rejected=True
            require(rejected,'audit accepted mutation '+field)
        faults.append(field)
    old=repo/'artifacts/reach_g4_actions_20260924/verification/final_01';oldfiles=load(old/'evidence.json')['files']
    for f,h in oldfiles.items():require(sha(record(old/f))==h,'historical G4-01 artifact changed')
    validation=load(source/'validation_metrics.json');stress=load(source/'stress_metrics.json')
    require(validation['supported']>100 and validation['outcomes']['command_use']['calibrated_n']>100,'no useful validation coverage')
    matched={}
    for j,name in enumerate(study.NAMES):
        xs=[(s,p) for s,p in zip(samples['validation'],predictions['validation']) if p['p'][j]>=0 and p['raw'][j]>=0]
        matched[name]=dict(n=len(xs),raw_brier=sum((p['raw'][j]-s['y'][j])**2 for s,p in xs)/len(xs) if xs else None,calibrated_brier=sum((p['p'][j]-s['y'][j])**2 for s,p in xs)/len(xs) if xs else None)
    by_case={};by_action={};latency={};strata={}
    for s in samples['train']:
        key=tuple(s['features'][:6]);count,sums=strata.get(key,(0,[0]*6));strata[key]=(count+1,[a+b for a,b in zip(sums,s['y'])])
    prior_baseline={}
    for j,name in enumerate(study.NAMES):
        pairs=[(s,p) for s,p in zip(samples['validation'],predictions['validation']) if p['raw'][j]>=0]
        losses=[]
        for s,p in pairs:
            count,sums=strata[tuple(s['features'][:6])];losses.append((sums[j]/count-s['y'][j])**2)
        prior_baseline[name]=dict(n=len(pairs),stratum_prior_brier=sum(losses)/len(losses) if losses else None,raw_local_brier=validation['outcomes'][name]['raw_brier'])
    for run in sorted(split_runs['validation']|split_runs['stress']):
        split='validation' if run in split_runs['validation'] else 'stress';pairs=[(s,p) for s,p in zip(samples[split],predictions[split]) if s['run']==run]
        by_case[report['cases'][run-1]['name']]=study.metrics([s for s,p in pairs],[p for s,p in pairs])
    keys=sorted({tuple(s['features'][:3]) for s in samples['validation']})
    for key in keys:
        pairs=[(s,p) for s,p in zip(samples['validation'],predictions['validation']) if tuple(s['features'][:3])==key]
        by_action[str(key)]=study.metrics([s for s,p in pairs],[p for s,p in pairs])
    for j,name in enumerate(('arrival','decoded','command_use')):
        for qi,q in enumerate((.5,.9)):
            pairs=[(s,p) for s,p in zip(samples['validation'],predictions['validation']) if p['raw'][0]>=0 and 0<=p['delays'][j*2+qi]<999]
            coverage=sum(s['delay'][j]<=p['delays'][j*2+qi] for s,p in pairs)/len(pairs) if pairs else None
            latency[name+str(q)]=dict(finite_quantile_n=len(pairs),coverage=coverage,nominal=q,coverage_error=coverage-q if coverage is not None else None,scope='finite predicted quantiles only; 999 means no finite quantile in horizon')
    aggregate=dict(passed=True,native_prediction_checks=native_tests['checks'],independent_kernel_checks=checks,mutation_checks=faults,split_counts={s:len(xs) for s,xs in samples.items()},validation=validation,stress=stress,matched_calibration=matched,latency_coverage=latency,by_case=by_case,by_action=by_action,stratum_prior_baseline=prior_baseline,historical_G4_01_files=len(oldfiles),scope='prediction plumbing and empirical calibration, not accuracy certification or superiority')
    save(out/'aggregate.json',aggregate)
    save(out/'task_decision.json',dict(task='G4-02',passed=True,completed_items=26,total_items=38,G4_completed=2,G4_total=5,passed_gates=4,total_gates=7,next='G4-03',scope='causal input plumbing, finite-window prediction and development calibration/error reporting',calibration_improvement_confirmed=False,policy_selection_connected=False,long_horizon_success_validated=False))
    # Freeze relevant sources after final review; captured trial-native sources above remain authoritative for collection.
    files=set(build['source_hashes'])|{'tools/reach_prediction_study.py','tools/verify_reach_prediction.py','tools/verify_reach_prediction_regression.py','tools/build_reach_prediction.py','tools/reach_prediction_probe.cpp','tools/reach_prediction_probe.vcxproj','tools/run_reach_g1.py','docs/Reach_RT_G4_Prediction.md'}
    hashes={}
    for f in sorted(files):
        if not (repo/f).exists():continue
        record(repo/f);target=out/'source'/f;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(repo/f,target);hashes[f]=sha(repo/f)
    save(out/'source_hashes.json',hashes);save(out/'inputs.json',inputs)
    trs=''.join('<tr><td>'+html.escape(name)+'</td><td>'+str(m['n'])+'</td><td>'+str(m['calibrated_n'])+'</td><td>'+('—' if m['raw_brier'] is None else f"{m['raw_brier']:.4f}")+'</td><td>'+('—' if m['calibrated_brier'] is None else f"{m['calibrated_brier']:.4f}")+'</td></tr>' for name,m in validation['outcomes'].items())
    page='''<!doctype html><html lang="ja"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Reach-RT G4-02</title><style>body{background:#eef3f8;color:#172e49;font:17px/1.7 system-ui;margin:0}main{max-width:1080px;margin:auto;padding:32px}header{background:#142c47;color:white;padding:28px;border-radius:16px}section{background:white;padding:24px;margin:20px 0;border-radius:14px}td,th{padding:10px;border-bottom:1px solid #dbe3ed;text-align:left}table{width:100%;border-collapse:collapse}a{color:#145bb5}strong{font-size:24px}</style><main><header><h1>G4-02 到着・作業価値の予測と校正</h1><p>到着済み情報 → 候補別の実測経路 → 200 ms先の予測 → 別試行で検証</p></header><section><strong>学習8試行 ／ 校正4試行 ／ 検証4試行 ／ ストレス2試行</strong><p>各20秒。全試行を分離し、保留検証の正解をモデルへ戻していません。予測は送信選択に未使用です。</p><p>予測値がない場合は−1（範囲外・通知失効・校正不足）。作業の60秒成功確率や方式の優位性を示す結果ではありません。</p></section><section><h2>保留した開発検証での誤差</h2><p>Brier scoreは小さいほど良好。校正前後で対象数が違う場合、値だけで改善とは判定しません。</p><table><tr><th>予測対象</th><th>範囲内数</th><th>校正値あり</th><th>校正前</th><th>校正後</th></tr>'''+trs+'''</table></section><section><h2>参照回復と作業利用を分ける</h2><p>画像が届いても復号・指令利用は別の事象です。IDR要求の生成待ちや、パリティ・再送を含む実測経路を保ち、失敗経路も含めて予測します。新規作業完了は200 ms内の観測事象で、単独操作の因果的な貢献とは呼びません。</p><p>次はG4-03：予測を使う有限探索、20 ms周期、計算打切りと代替処理。</p><p><a href="aggregate.json">誤差・監査JSON</a> / <a href="source/docs/Reach_RT_G4_Prediction.md">実装説明</a> / <a href="inputs.json">証拠ハッシュ</a></p></section></main></html>'''
    (out/'index.html').write_text(page,encoding='utf-8')
    save(out/'evidence.json',dict(files={p.relative_to(out).as_posix():sha(p) for p in out.rglob('*') if p.is_file()}))
    print(json.dumps(aggregate,ensure_ascii=False));return 0
if __name__=='__main__':sys.exit(main())
