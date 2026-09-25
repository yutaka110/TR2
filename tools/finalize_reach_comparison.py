"""Freeze G4-04 evidence; preserve negative results, old gates and conditional scope."""
import argparse,hashlib,html,json,re,shutil
from collections import Counter
from fractions import Fraction as F
from pathlib import Path
from verify_reach_closed_loop import read,rows,save
from verify_reach_command import require
from verify_reach_scheduler import fault_checks
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def number(v):return v['decimal'] if isinstance(v,dict) else v
def main():
    p=argparse.ArgumentParser();p.add_argument('--name',default='final_01');p.add_argument('--build-name',default='reach_g4_comparison_20260925');p.add_argument('--build',default='build_01');p.add_argument('--study',default='study_01');a=p.parse_args()
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/a.build_name;out=root/'verification'/a.name;out.mkdir(parents=True,exist_ok=False);inputs={}
    def record(p):
        p=p.resolve();require(p.is_relative_to(repo),'outside repository');inputs[p.relative_to(repo).as_posix()]=sha(p);return p
    def load(p):return read(record(p))
    build=load(root/'verification'/a.build/'report.json');study=load(root/'verification'/a.study/'report.json');plan=load(root/'verification'/a.study/'plan.json');selection=load(root/'verification'/a.study/'selection.json')
    exact=load(root/'verification/exact_01/report.json');analysis=load(root/'verification/analysis_01/report.json');regression=load(root/'verification/comparison_regression_01/report.json')
    require(all(r['passed'] for r in (build,study,exact,analysis,regression)),'incomplete verification');require(len(study['trials'])==104 and study['complete'],'trial count')
    require(sha(record(root/'bin/Release/GE3.exe'))==build['executable_sha256']==plan['executable_sha256'],'wrong executable')
    require(selection['plan_sha256']==sha(root/'verification'/a.study/'plan.json') and not selection['holdout_used'],'selection lock')
    for f,h in build['source_hashes'].items():require(sha(record(repo/f))==h,'native source changed')
    for mode in plan['tuned_modes']:
        recalculated=[]
        for weight in plan['lambdas']:
            ts=[t for t in study['trials'] if t['phase']=='tuning' and t['mode']==mode and t['lambda']==weight];require(len(ts)==2 and all(t['passed'] for t in ts),'unequal tuning budget')
            recalculated.append(dict(**{'lambda':weight},successes=sum(t['integration']['motion']['outcome']=='success' for t in ts),terminal_seconds=sum(t['integration']['motion']['terminal']['simulation_s'] if t['integration']['motion']['outcome']=='success' else 20 for t in ts),ip_bytes=sum(t['budget']['summary']['attempted_ip_bytes'] for t in ts)))
        best=min(recalculated,key=lambda s:(-s['successes'],s['terminal_seconds'],s['ip_bytes'],plan['lambdas'].index(s['lambda'])))
        require(selection['selection'][mode]==dict(chosen=best,all_scores=recalculated),'retuned on validation')
    diagnostic_proof=[];overruns=[]
    for t in study['trials']:
        session=Path(t['session']);run=session.parent.parent;launch=load(run/'launch.json');require(launch['exit_code']==0 and launch['executable_sha256']==build['executable_sha256'],'trial invalid/build changed')
        hashes=load(run/'source_hashes.json')
        for f,h in build['source_hashes'].items():require(hashes[f]==h and sha(record(run/'research_source'/f))==h,'trial source mismatch')
        for f in session.iterdir():
            if f.is_file():record(f)
        cfg=load(run/'requested_config.json');case=next(c for c in plan['cases'] if c['name']==t['case']);require(cfg['ip_budget']==case['budget'] and cfg['link_model']==case['link'] and cfg['state_feedback'],'unequal resources or notification cost')
        if t['phase']=='validation':require(t['started']>selection['frozen'],'validation before tuning lock');require(t['lambda']==(.1 if t['mode']=='B0' else selection['selection'][t['mode']]['chosen']['lambda']),'unlocked lambda')
        if (run/'prediction_paths.csv').exists():require(sha(record(run/'prediction_paths.csv'))==plan['model_sha256'],'fit changed')
        if (run/'prediction_paths.csv.cal').exists():record(run/'prediction_paths.csv.cal')
        if 'scheduler' in t and t['scheduler']['selection_over_5ms']:
            for tick in rows(session/'scheduler.csv'):
                if int(tick['selection_us'])>5000:overruns.append(dict(trial=t['id'],tick=tick))
        if t['phase']=='validation':
            proof=dict(trial=t['id'],case=t['case'])
            if t['case'] in ('idr_missing','p_missing'):
                fid='1' if t['case']=='idr_missing' else '30';frame=next(r for r in rows(session/'encoded.csv') if r['frame_id']==fid)
                require(frame['diagnostic_drop']=='1' and frame['idr']==('1' if fid=='1' else '0'),'omitted AU type mismatch');proof['omitted_frame']=fid;proof['actual_idr']=int(frame['idr'])
            if t['case']=='recognition_stall':
                timings=[int(r['recognition_us']) for r in rows(session/'decode_timing.csv') if r['frame_id']=='60'];require(timings and max(timings)>=250000,'recognition stall not exercised');proof['recognition_us']=max(timings)
            if t['case']=='notification_reorder':
                n=t['state']['summary']['statuses'].get('reordered',0);require(n>0,'notification reorder not exercised');proof['reordered_notifications']=n
            if t['case']=='notification_blackout':
                world=rows(session/'world.csv');require(max(float(w['v_m_s']) for w in world)>.05 and all(float(w['v_m_s'])==0 for w in world if float(w['simulation_s'])>4),'blackout braking not exercised');proof['stopped_after_4s']=True
            if t['case']=='compute_cutoff':require(t['scheduler']['reasons'].get('compute_budget',0)>0,'cutoff not exercised');proof['synthetic_cutoffs']=t['scheduler']['reasons']['compute_budget']
            if len(proof)>2:diagnostic_proof.append(proof)
    units=load(root/'verification/regression_units_01/report.json');require(units['passed'],'old units');unit_count=sum(int(re.match(r'PASS (\d+)',t['output']).group(1)) for t in units['tests'])
    for f in ('scheduler_tests.json','prediction_tests.json','comparison_tests.json','command_1.txt'):
        r=load(root/'verification'/a.build/f);require(r['passed'],'native units');unit_count+=r['checks']
    require(unit_count==3080,'unit total changed')
    # In-memory mutation: never edit original evidence.
    probe_trial=next(t for t in study['trials'] if t['phase']=='validation' and t['case']=='normal_T2' and t['mode']=='joint');mutations=fault_checks(Path(probe_trial['session']))
    old=repo/'artifacts/reach_g4_scheduler_20260925/verification/final_01';historical=load(old/'evidence.json')['files']
    for f,h in historical.items():require(sha(record(old/f))==h,'G4-03 frozen evidence changed')
    for folder in (a.study,'analysis_01','exact_01','comparison_regression_01','scheduler_regression_01','regression_01','actions_regression_01','legacy_regression_01','regression_units_01',a.build):
        for f in (root/'verification'/folder).rglob('*'):
            if f.is_file() and f.suffix in ('.json','.jsonl','.csv','.txt','.html'):record(f)
    for name in ('g403_regression_01','g402_regression_01','b3_regression_01'):
        for f in (root/'runs'/name).rglob('*'):
            if f.is_file() and f.suffix in ('.json','.csv','.jsonl'):record(f)
    schedules=[t['scheduler'] for t in study['trials'] if 'scheduler' in t];validation=[t for t in study['trials'] if t['phase']=='validation'];primary=[t for t in validation if t['duration_s']==20]
    exact_groups={}
    for row in exact['rows']:exact_groups.setdefault(row['case'],{})[row['mode']]=row
    exact_information=[]
    for case,modes in exact_groups.items():
        for left,right in (('B2','B1'),('B3','B1'),('JOINT','B2'),('JOINT','B3'),('JOINT','B1')):
            exact_information.append(dict(case=case,comparison=left+' vs '+right,optimal_delta=number(modes[left]['optimal'])-number(modes[right]['optimal']),proxy_delta=number(modes[left]['proxy'])-number(modes[right]['proxy'])))
    aggregate=dict(passed=True,native_checks=unit_count,trial_count=104,tuning_count=32,validation_count=72,primary_task_trials=len(primary),diagnostic_trials=len(validation)-len(primary),
        native_ticks=sum(s['ticks'] for s in schedules),max_case_p95_us=max(s['selection_p95_us'] for s in schedules),max_case_p99_us=max(s['selection_p99_us'] for s in schedules),
        timing_target_failed_trials=[t['id'] for t in study['trials'] if 'scheduler' in t and (t['scheduler']['selection_p95_us']>2000 or t['scheduler']['selection_p99_us']>5000)],
        skipped=sum(s['skipped'] for s in schedules),selection_over_5ms=sum(s['selection_over_5ms'] for s in schedules),mutations=mutations,
        exact=dict(cases=len(exact['rows']),mean_regret=exact['mean_regret'],max_regret=exact['max_regret'],equal_rows=exact['equal_rows'],worse_rows=exact['worse_rows'],independent_exact=exact['independent_exact'],independent_proxy=exact['independent_proxy'],native_rank_queries=exact['native_rank_queries']),
        exact_information=exact_information,diagnostic_proof=diagnostic_proof,
        same_snapshot=dict(snapshots=analysis['snapshots'],queries=analysis['native_queries'],live_matches=analysis['live_replay_matches'],cutoff_excluded=analysis['live_cutoff_excluded']),
        contrast_classes=dict(Counter(c['classification'] for c in analysis['contrasts'])),primary_contrast_classes=dict(Counter(c['classification'] for c in analysis['contrasts'] if c['case'] in {t['case'] for t in primary})),overruns=overruns,
        independent_prediction_checks=sum(t.get('kernel',{}).get('independent_queries',0) for t in study['trials']),
        live=analysis['live'],contrasts=analysis['contrasts'],same_snapshot_comparisons=analysis['same_snapshot'],historical_G4_03_files=len(historical),
        scope='exploratory condition-wise comparisons, one trial per cell; exact regret is finite-model only; no 60s/G5/statistical superiority claim')
    save(out/'aggregate.json',aggregate);save(out/'task_decision.json',dict(task='G4-04',passed=True,completed_items=28,total_items=38,G4_completed=4,G4_total=5,passed_gates=4,total_gates=7,next='G4-05',scope='condition-wise exact/proxy regret, equal-candidate information ablation and equal-opportunity tuning',superiority_validated=False,system_gate_passed=False,pilot_lock_completed=False))
    files=set(build['source_hashes'])|{str(f.relative_to(repo)).replace('\\','/') for f in (repo/'tools').glob('*reach*comparison*')}|{'tools/reach_scheduler_exact.py','tools/run_reach_scheduler_exact.py','tools/reach_exact_model.py','tools/test_reach_exact.py','tools/run_reach_exact_study.py','tools/verify_reach_scheduler.py','tools/run_reach_g1.py','docs/Reach_RT_G4_Comparison.md'}
    hashes={}
    for f in sorted(files):
        if not (repo/f).is_file():continue
        record(repo/f);target=out/'source'/f;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(repo/f,target);hashes[f]=sha(repo/f)
    save(out/'source_hashes.json',hashes);save(out/'inputs.json',inputs)
    table=[]
    for case,modes in analysis['live'].items():
        for mode,v in modes.items():table.append('<tr>'+''.join('<td>'+html.escape(str(x))+'</td>' for x in (case,mode,v['lambda'],v['outcome'],v['seconds'],v['duration_s'],v['ip_bytes'],round(v['fallback_rate']*100,1) if v['fallback_rate'] is not None else '—'))+'</tr>')
    worst=max(exact['rows'],key=lambda r:number(r['regret']));witness=max((d for r in exact['rows'] for d in r['decisions']),key=lambda d:number(d['one_step_regret']))
    page='''<!doctype html><html lang="ja"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Reach-RT G4-04 Comparison</title><style>body{font:16px/1.7 system-ui;background:#edf2f7;color:#19324b;margin:0}main{max-width:1250px;margin:auto;padding:28px}header{background:#132e48;color:white;padding:28px;border-radius:14px}section{background:white;padding:24px;margin:18px 0;border-radius:12px}table{border-collapse:collapse;width:100%;white-space:nowrap}td,th{padding:8px;text-align:left;border-bottom:1px solid #dbe3ed}.scroll{overflow:auto}strong{font-size:26px}select{font:inherit;padding:7px}pre{white-space:pre-wrap}a{color:#165ca9}</style><main><header><h1>G4-04 厳密解との比較・アブレーション</h1><p>同じ候補で情報だけを変える比較と、実際の閉ループの方式比較を分けます。</p></header><section><strong>調整32試行 ／ 固定後72試行 ／ 単体3,080検査</strong><p>全条件は探索的な各1試行。主な作業比較は20秒、停止・範囲外等の診断は8秒です。成功率の有意差や60秒の性能保証を示す結果ではありません。</p></section><section><h2>予測が正確でも、短期判断は長期最適と一致しない</h2><p>有限モデル160比較中、同等82、悪化78。平均成功確率差1.35406ポイント、最大7.03125ポイント。因果的な同情報の最適解と比較しています。</p><p>二段階の作業で、最初の画像に全予算を使うと後続画像を送れません。直近画像の利用確率を高めることだけでは最終成功を最大化できません。実RNVPの成功率差ではありません。</p><details><summary>記録された反例の判断を表示</summary><pre>'''+html.escape(json.dumps(witness,ensure_ascii=False,indent=2))+'''</pre></details></section><section><h2>実通信の条件別結果</h2><p>失敗時の秒は実験窓の終了時刻です。失敗同士の通信節約を作業改善とは扱いません。B0～B3は作用空間も異なるシステム比較です。</p><label>条件 <select id="filter"><option value="">すべて</option></select></label><div class="scroll"><table id="results"><thead><tr>'''+''.join('<th>'+x+'</th>' for x in ('条件','方式','λ','作業結果','秒','実験窓','IP byte','代替%'))+'''</tr></thead><tbody>'''+''.join(table)+'''</tbody></table></div></section><section><h2>次に固定すべきこと</h2><p>短期スコアを完成した長期最適方策とは扱わず、予算を後続作業へ残す設計と、範囲外・代替の影響を分けて検討します。次工程G4-05で、継続する仮説、主比較、反復数、無効条件を固定します。</p><p><a href="aggregate.json">結果・全比較JSON</a> / <a href="task_decision.json">工程判定</a> / <a href="source/docs/Reach_RT_G4_Comparison.md">実装と限界</a> / <a href="inputs.json">証拠ハッシュ</a></p></section><script>const select=document.getElementById('filter'),rows=[...document.querySelectorAll('#results tbody tr')];[...new Set(rows.map(r=>r.cells[0].textContent))].forEach(c=>{const o=document.createElement('option');o.value=o.textContent=c;select.append(o)});select.onchange=()=>rows.forEach(r=>r.hidden=!!select.value&&r.cells[0].textContent!==select.value);</script></main></html>'''
    extra='<section><h2>有効例・悪化例・無効果例</h2><p>通常T1：jointは17.92秒で完了、commonは20秒以内に未完了。通常T2：jointは11.96秒、commonは10.59秒で、jointが1.37秒遅い結果でした。小キュー複合障害：既存方式を含む9方式すべて未完了。いずれも各1試行の観測で、因果効果・統計的優位性の確定ではありません。</p><p>調整条件で5マスク方式はいずれも成功0件だったため、λは通信量の同率判定で決まりました。成功改善を確認した調整とはいえません。</p></section>'
    extra+='<section><h2>同じ候補で情報だけを変更</h2><p>1,338状態を8,028回評価。λ=0.1固定で比べ、実測時の係数で1,225状態の選択を再現しました。時間打切り113状態は再現一致の対象から分離しています。表はjointとcommonの比較です。両方で予測が使えた場合の判断差も分けています。</p><div class="scroll"><table><thead><tr><th>条件</th><th>状態数</th><th>判断差</th><th>双方で予測支持</th><th>その中の判断差</th></tr></thead><tbody>'
    for case,pairs in analysis['same_snapshot'].items():
        v=pairs['joint vs common'];extra+='<tr>'+''.join('<td>'+html.escape(str(x))+'</td>' for x in (case,v['n'],v['different_action'],v['mutual_supported'],v['different_with_mutual_support']))+'</tr>'
    extra+='</tbody></table></div><p>判断差は確認できますが、別の行動を続けた軌跡や作業結果を生成する再生ではありません。</p></section>'
    extra+='<section><h2>計算時間と確認範囲</h2><p>61,920判断、周期スキップ0。試行別p95の最大1.901 ms、p99の最大3.195 ms。個別には5 ms超過が5件あり、協調打切りを硬い時間保証とは扱いません。診断注入と実際の超過は別記録です。</p><p>G4-04完了：全体28/38項目、G4は4/5、研究ゲート4/7。G4全体・Rの優位性は未判定です。</p></section>'
    page=page.replace('<section><h2>次に固定すべきこと</h2>',extra+'<section><h2>次に固定すべきこと</h2>').replace('href="source/docs/Reach_RT_G4_Comparison.md"','href="../../../../docs/Reach_RT_G4_Comparison.md"')
    (out/'index.html').write_text(page,encoding='utf-8');save(out/'evidence.json',dict(files={p.relative_to(out).as_posix():sha(p) for p in out.rglob('*') if p.is_file()}));print(json.dumps({k:v for k,v in aggregate.items() if k not in ('live','contrasts','same_snapshot_comparisons','exact_information','diagnostic_proof','overruns')},ensure_ascii=False));return 0
if __name__=='__main__':raise SystemExit(main())
