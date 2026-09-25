"""Freeze attributable G4-03 implementation evidence and its local result viewer."""
import argparse,hashlib,html,json,re,shutil,sys
from pathlib import Path
from verify_reach_closed_loop import read,rows,save
from verify_reach_command import require
from verify_reach_scheduler import scheduler_audit
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser();p.add_argument('--name',default='final_01');p.add_argument('--build-name',default='reach_g4_scheduler_20260925');a=p.parse_args()
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/a.build_name;out=root/'verification'/a.name;out.mkdir(parents=True,exist_ok=False);inputs={}
    def record(p):
        p=p.resolve();require(p.is_relative_to(repo),'outside repository');inputs[p.relative_to(repo).as_posix()]=sha(p);return p
    def load(p):return read(record(p))
    build=load(root/'verification/build_03/report.json');require(build['passed'],'build failed');require(sha(record(root/'bin/Release/GE3.exe'))==build['executable_sha256'],'executable changed')
    for f,h in build['source_hashes'].items():require(sha(record(repo/f))==h,'native source changed after build: '+f)
    matrix=load(root/'verification/matrix_01/report.json');regression=load(root/'verification/scheduler_regression_01/report.json');older=load(root/'verification/regression_01/report.json')
    units=load(root/'verification/regression_units_01/report.json');require(all(r['passed'] for r in (matrix,regression,older,units)),'incomplete acceptance')
    require(len(matrix['cases'])==10,'missing matrix case')
    native=load(root/'verification/build_03/scheduler_tests.json');prediction=load(root/'verification/build_03/prediction_tests.json');actions=load(root/'verification/build_03/command_1.txt')
    require(native['passed'] and native['checks']==233 and prediction['passed'] and prediction['checks']==37 and actions['passed'] and actions['checks']==684,'native test counts changed')
    native_count=sum(int(re.match(r'PASS (\d+)',t['output']).group(1)) for t in units['tests'])
    require(native_count==2000,'legacy unit count changed')
    for folder in ('build_03','regression_units_01','actions_regression_01','legacy_regression_01','regression_01','scheduler_regression_01','matrix_01'):
        for f in (root/'verification'/folder).rglob('*'):
            if f.is_file() and f.suffix not in ('.obj','.pdb','.exe','.lib','.tlog','.log','.ipdb','.iobj'):record(f)
    frozen=repo/'artifacts/reach_g4_prediction_20260924/verification/final_01';historical=load(frozen/'evidence.json')['files']
    for f,h in historical.items():require(sha(record(frozen/f))==h,'G4-02 historical evidence changed')
    model=repo/'artifacts/reach_g4_prediction_20260924/verification/study_02/paths.csv';record(model);record(Path(str(model)+'.cal'))
    trials=[(c,True) for c in matrix['cases']]
    actions_report=load(root/'verification/actions_regression_01/report.json');require(actions_report['passed'],'G401 regression')
    trials += [(c,False) for c in actions_report['cases']]+[(older['B3'],False),(regression['G4-02'],False)]
    for c,is_scheduler in trials:
        s=Path(c['session']);run=s.parent.parent;launch=load(run/'launch.json');require(launch['exit_code']==0 and launch['executable_sha256']==build['executable_sha256'],'trial/executable mismatch')
        hashes=load(run/'source_hashes.json')
        for f,h in build['source_hashes'].items():require(hashes[f]==h and sha(record(run/'research_source'/f))==h,'trial source mismatch')
        for f in s.iterdir():
            if f.is_file():record(f)
        record(run/'requested_config.json')
        if (run/'prediction_paths.csv').exists():require(sha(record(run/'prediction_paths.csv'))==sha(model),'trial model changed')
        if (run/'prediction_paths.csv.cal').exists():require(sha(record(run/'prediction_paths.csv.cal'))==sha(Path(str(model)+'.cal')),'trial calibration changed')
        if is_scheduler:require(scheduler_audit(s)==c['scheduler'],'scheduler re-audit changed')
    aggregate=dict(passed=True,native_scheduler_checks=233,native_prediction_checks=37,native_action_checks=684,existing_native_checks=2000,total_native_checks=2954,
        real_scheduler_cases=len(matrix['cases']),ticks=sum(c['scheduler']['ticks'] for c in matrix['cases']),candidate_rows=sum(c['scheduler']['candidate_rows'] for c in matrix['cases']),
        skipped=sum(c['scheduler']['skipped'] for c in matrix['cases']),selection_over_5ms=sum(c['scheduler']['selection_over_5ms'] for c in matrix['cases']),
        max_case_p95_us=max(c['scheduler']['selection_p95_us'] for c in matrix['cases']),max_case_p99_us=max(c['scheduler']['selection_p99_us'] for c in matrix['cases']),
        independent_kernel_checks=sum(c.get('kernel',{}).get('independent_queries',0) for c in matrix['cases']),mutation_checks=next(c['mutation_checks'] for c in matrix['cases'] if 'mutation_checks' in c),
        cases=matrix['cases'],regressions='G4-01 seven UDP cases, G4-02, B3, legacy display, 2000 native checks',historical_G4_02_files=len(historical),
        timing_scope='snapshot + bounded selection + revalidation + authorization log; dispatch measured separately; candidate detail log formatting excluded; not hard realtime',
        development_trials='pilot_01 on build_01 retained separately; build_02 had no live trials',auditor_correction=matrix.get('auditor_correction'))
    save(out/'aggregate.json',aggregate)
    save(out/'task_decision.json',dict(task='G4-03',passed=True,completed_items=27,total_items=38,G4_completed=3,G4_total=5,passed_gates=4,total_gates=7,next='G4-04',scope='20ms bounded scheduling, cooperative 5ms cutoff, causal raw-proxy selection, revalidated common recovery',calibration_used_for_selection=False,changed_policy_accuracy_validated=False,hard_realtime_guarantee=False,long_horizon_success_validated=False,superiority_validated=False))
    files=set(build['source_hashes'])|{str(f.relative_to(repo)).replace('\\','/') for f in (repo/'tools').glob('*reach*scheduler*')}|{'tools/run_reach_g1.py','docs/Reach_RT_G4_Scheduler.md'}
    hashes={}
    for f in sorted(files):
        if not (repo/f).is_file():continue
        record(repo/f);target=out/'source'/f;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(repo/f,target);hashes[f]=sha(repo/f)
    save(out/'source_hashes.json',hashes);save(out/'inputs.json',inputs)
    data=[]
    for c in matrix['cases']:
        s=Path(c['session']);data.append(dict(name=c['name'],timing=c['scheduler'],ticks=[{k:r[k] for k in ('tick','kind','frame_id','fallback','reason','revalidation','selection_us','dispatch_us','decision_id','notification_sequence')} for r in rows(s/'scheduler.csv')]))
    trs=''.join(f'<tr><td>{html.escape(c["name"])}</td><td>{c["scheduler"]["ticks"]}</td><td>{c["scheduler"]["selection_p95_us"]/1000:.3f}</td><td>{c["scheduler"]["selection_p99_us"]/1000:.3f}</td><td>{c["scheduler"]["skipped"]}</td><td>合格</td></tr>' for c in matrix['cases'])
    page='''<!doctype html><html lang="ja"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Reach-RT G4-03 | Scheduler</title><style>
body{margin:0;background:#edf2f7;color:#172e49;font:16px/1.7 system-ui}main{max-width:1150px;margin:auto;padding:28px}header{background:#142c47;color:white;padding:30px;border-radius:16px}h1{margin:0}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:16px;margin:20px 0}.card,section{background:white;padding:22px;border-radius:12px}strong{font-size:28px}.note{color:#5a6071}section{margin:20px 0}td,th{padding:8px;text-align:left;border-bottom:1px solid #dbe3ed}table{width:100%;border-collapse:collapse}.scroll{overflow:auto}select{font:inherit;padding:8px;width:100%}#timeline{display:flex;flex-wrap:wrap;gap:3px;margin:20px 0}.tick{width:10px;height:22px;padding:0;border:0;border-radius:2px;cursor:pointer}.tick:focus{outline:3px solid #172e49}.proxy{background:#13846d}.fallback{background:#286cbe}.defer{background:#bdc8d3}.cut{background:#df8f25}pre{white-space:pre-wrap;background:#f3f6fa;padding:18px}a{color:#145bb5}</style><main>
<header><h1>G4-03 ReachScheduler</h1><p>到着済みの情報 → 20 msごとの候補選択 → 期限・世代・予算の再確認 → 実UDP</p><p>実装・障害条件の検証結果。研究上の性能優位を示す結果ではありません。</p></header>
<div class="grid"><div class="card"><strong>20 ms</strong><div>判断の予定周期</div></div><div class="card"><strong>5 ms</strong><div>協調的な計算打切り</div></div><div class="card"><strong>10 / 10</strong><div>実通信条件の監査合格</div></div><div class="card"><strong>2,954</strong><div>C++単体検査</div></div></div>
<section><h2>実測した判断時間</h2><p>単位ms。p95≤2 ms・p99≤5 msは今回の実測目標です。OS上の時間保証ではありません。RNVP投入と詳細ログ整形は別処理です。</p><div class="scroll"><table><tr><th>条件</th><th>判断数</th><th>p95</th><th>p99</th><th>周期スキップ</th><th>監査</th></tr>'''+trs+'''</table></div></section>
<section><h2>判断の流れを確認する</h2><label for="case">条件</label><select id="case"></select><p>1本が1判断。緑：予測の順位付け、青：共通回復、灰：見送り、橙：打切り。クリックすると記録が表示されます。</p><div id="timeline"></div><pre id="detail">判断を選択してください。</pre></section>
<section><h2>校正結果の限界を実装へ反映</h2><p>前工程で校正後の誤差改善が確認できなかったため、生の短期予測を実験用の順位付けにだけ使用します。候補の支持不足・古い通知・計算打切りでは、受信済み要求に基づく再送、IDR更新、新規画像、見送りへ戻ります。</p><p>timeout条件は人工的に計算予算を使い切ったと通知する診断で、実CPU過負荷の成績ではありません。5 msを超える実処理や開始遅れは監査JSONに別記します。</p><p>次はG4-04：厳密解・情報マスク・係数調整対照との比較。方策変更後の予測精度、作業成功率、Rの優位性は未検証です。</p><p><a href="aggregate.json">全監査JSON</a> / <a href="task_decision.json">工程判定</a> / <a href="source/docs/Reach_RT_G4_Scheduler.md">実装説明</a> / <a href="inputs.json">証拠ハッシュ</a></p></section>
<script>const data='''+json.dumps(data,ensure_ascii=False).replace('</',r'<\/')+''';const select=document.getElementById('case');data.forEach((c,i)=>{const o=document.createElement('option');o.value=i;o.textContent=c.name;select.append(o)});function show(){const c=data[+select.value],host=document.getElementById('timeline');host.replaceChildren();document.getElementById('detail').textContent=JSON.stringify(c.timing,null,2);c.ticks.forEach(t=>{const b=document.createElement('button');b.className='tick '+(t.reason==='compute_budget'?'cut':t.kind==='DEFER'?'defer':t.fallback==='1'?'fallback':'proxy');b.title='tick '+t.tick+' / '+t.kind+' / '+t.reason;b.setAttribute('aria-label',b.title);b.onclick=()=>document.getElementById('detail').textContent=JSON.stringify(t,null,2);host.append(b)})}select.onchange=show;show();</script></main></html>'''
    (out/'index.html').write_text(page,encoding='utf-8');save(out/'evidence.json',dict(files={p.relative_to(out).as_posix():sha(p) for p in out.rglob('*') if p.is_file()}))
    print(json.dumps({k:v for k,v in aggregate.items() if k!='cases'},ensure_ascii=False));return 0
if __name__=='__main__':sys.exit(main())
