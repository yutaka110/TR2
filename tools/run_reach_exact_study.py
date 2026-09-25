"""G3-03 offline exact study: fixed grid, independent checks, native probe, artifacts."""
import argparse,hashlib,html,json,os,platform,shutil,subprocess,sys,time
from dataclasses import asdict,replace
from datetime import datetime
from fractions import Fraction as F
from pathlib import Path
from reach_exact_model import Model,Exact,MODES,hindsight,full_state_causal
from reach_exact_transport import hairpin_checks,native_checks

FILES=('tools/reach_exact_model.py','tools/test_reach_exact.py','tools/reach_exact_transport.py',
       'tools/run_reach_exact_study.py','tools/reach_planner_probe.cpp','tools/reach_planner_probe.vcxproj',
       'research/ReachBaselinePlanner.h','research/ReachBaselineState.h','research/ReachStateProtocol.h')


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def encode(obj):
    if isinstance(obj,F):return dict(exact=str(obj),decimal=float(obj))
    raise TypeError(type(obj).__name__)
def save(path,obj):path.write_text(json.dumps(obj,default=encode,ensure_ascii=False,indent=2),encoding='utf-8')


def render(out,rows,transport):
    base=next(x for x in rows if x['name']=='reference_case')
    cards=''.join(f'<article><h2>{m}情報の最適解</h2><b>{100*float(base["exact"][m]["success"]):.3f}%</b><p>{base["exact"][m]["success"]}</p></article>' for m in MODES)
    table=[]
    for row in rows:
        fields=[row['name'],row['model']['horizon'],row['model']['budget_packets'],row['model']['delay'],str(row['model']['report_loss'])]
        fields += [f'{100*float(row["exact"][m]["success"]):.3f}' for m in MODES]
        fields += [f'{100*float(row["hindsight"]["success"]):.3f}',max(x['nodes'] for x in row['exact'].values())]
        table.append('<tr>'+''.join('<td>'+html.escape(str(x))+'</td>' for x in fields)+'</tr>')
    budget_rows=[]
    for b in range(6):
        case=next(x for x in rows if x['name']==f'grid_h3_b{b}_d0_q0')
        cells=[f'<td>{b}</td>']
        for mode in MODES:
            value=case['exact'][mode]['success'];shade=round(96-25*float(value))
            cells.append(f'<td style="background:hsl(190 50% {shade}%)">{100*float(value):.3f}%<br><small>{value}</small></td>')
        budget_rows.append('<tr>'+''.join(cells)+'</tr>')
    budget_table='<p>3スロット・即時通知・欠落なし。予算はデータパケット数。通知256 IP byteを別途全方式で予約。</p><table><tr><th>予算</th>'+''.join('<th>'+m+'</th>' for m in MODES)+'</tr>'+''.join(budget_rows)+'</table>'
    (out/'budget_values.html').write_text('<!doctype html><html lang="ja"><meta charset="utf-8"><title>Budget and exact value</title>'+budget_table+'</html>',encoding='utf-8')
    worst=max(transport['grid'],key=lambda r:r['objective_gap'])
    body=f'''<!doctype html><html lang="ja"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>Reach-RT G3-03</title>
<style>body{{font:16px system-ui;color:#17334a;background:#edf3f8;max-width:1400px;margin:32px auto;padding:0 24px}}section,article{{background:white;border-radius:12px;padding:22px;margin:16px 0}}.cards{{display:flex;gap:16px;flex-wrap:wrap}}article{{flex:1;min-width:180px}}b{{font-size:32px}}table{{border-collapse:collapse;white-space:nowrap}}td,th{{padding:9px;border-bottom:1px solid #dce5ef;text-align:right}}.scroll{{overflow:auto}}a{{color:#087e8b}}img{{max-width:800px;width:100%}}</style>
<h1>Reach-RT / 情報の価値と厳密解</h1><p>有限モデルの計算結果です。実アプリの成功率や、提案方式Rの実測ではありません。</p>
<p>基準例：3スロット・データ3パケット分・即時通知・通知欠落なし。各割合は標本の成功率ではなく、全状態・全事象から求めた厳密な確率です。</p>
<div class="cards">{cards}</div><section><h2>観測と未来を分ける</h2><p>B1：配送通知のみ / B2：＋参照状態 / B3：＋作業段階 / JOINT：両方。すべて同じ候補と予算で因果的に最適化します。</p>
<p>全現在状態を知る因果的上限：{base['full_state']}。未来も知る事後オラクル：{base['hindsight']['success']}。オラクルは実用方式との比較には使いません。</p>{budget_table}</section>
<section><h2>現行C++の近似を検算</h2><p>実際の13候補関数を別の小さな実行ファイルに組み込み、{len(transport['grid'])}条件を比較しました。独立損失・余裕のある期限で配送確率が一致した候補は{transport['iid_probability_equal_checks']}件です。</p>
<p>宣言した単一画像・一括配送モデルでの最大目的値差：{float(worst['objective_gap']):.6f}。閉ループの作業成功率差とは異なります。生データに全候補と正確な分数を保存しています。</p></section>
<section><h2>全条件</h2><p>通知費用も全方式で共通に予約。qは通知欠落確率、dは通知遅延スロットです。検索は表示行だけを絞ります。</p><input id="filter" placeholder="条件名で検索" aria-label="条件名で検索"><div class="scroll"><table><thead><tr>{''.join('<th>'+s+'</th>' for s in ('条件','H','予算','d','q','B1 %','B2 %','B3 %','JOINT %','事後上限 %','最大ノード数'))}</tr></thead><tbody>{''.join(table)}</tbody></table></div></section>
<p><a href="report.json">全計算と判断</a> / <a href="plan.json">事前の条件一覧</a> / <a href="transport.json">現行近似の検算</a> / <a href="hairpin.json">公開式の小規模照合</a> / <a href="task_decision.json">完了判定</a></p>
<script>document.getElementById('filter').addEventListener('input',e=>document.querySelectorAll('tbody tr').forEach(r=>r.hidden=!r.textContent.toLowerCase().includes(e.target.value.toLowerCase())));</script></html>'''
    (out/'index.html').write_text(body,encoding='utf-8')


def main():
    p=argparse.ArgumentParser();p.add_argument('--name',default='study_01');p.add_argument('--output-root',default='artifacts/reach_g3_exact_20260924');a=p.parse_args()
    repo=Path(__file__).resolve().parents[1];out=(repo/a.output_root/a.name).resolve()
    if not out.is_relative_to(repo/'artifacts'):raise ValueError('output must stay in workspace artifacts')
    out.mkdir(parents=True,exist_ok=False)
    dependencies=set(FILES)|{str(f.relative_to(repo)).replace('\\','/') for folder in ('research','network') for f in (repo/folder).glob('*.h')}
    hashes={f:sha(repo/f) for f in sorted(dependencies)};save(out/'source_hashes.json',hashes)
    sources=out/'source';sources.mkdir()
    for f in hashes:
        dest=sources/f;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(repo/f,dest)
    cases=[('reference_case',Model())]
    for h,b,d,q in product_grid():cases.append((f'grid_h{h}_b{b}_d{d}_q{q}',Model(horizon=h,budget_packets=b,delay=d,report_loss=q)))
    cases += [('known_initial',Model(initial=((1,0,0,F(1,2)),(1,0,1,F(1,2))))),
              ('size_h4',Model(horizon=4,budget_packets=4,delay=1,report_loss=F(1,2)))]
    save(out/'plan.json',dict(version='G3-03-v1',created_at=datetime.now().astimezone().isoformat(),cases=[dict(name=n,model=asdict(m)) for n,m in cases],
        policies=['exact per mask','one-step progress minus 0.1 per packet'],exact_node_limit=200000,
        claim='finite model only; no real-time performance, R implementation, or statistical result',source_hashes=hashes))
    report=dict(passed=False,version='G3-03-v1',python=sys.version,platform=platform.platform(),cases=[])
    try:
        tests=subprocess.run([sys.executable,str(repo/'tools/test_reach_exact.py')],capture_output=True)
        (out/'tests.txt').write_bytes(tests.stdout+tests.stderr)
        if tests.returncode:raise RuntimeError('independent exact tests failed')
        vswhere=Path(os.environ['ProgramFiles(x86)'])/'Microsoft Visual Studio/Installer/vswhere.exe'
        msbuild=subprocess.check_output([str(vswhere),'-latest','-products','*','-requires','Microsoft.Component.MSBuild','-find','MSBuild/**/Bin/MSBuild.exe'],text=True).splitlines()[0]
        build=subprocess.run([msbuild,str(repo/'tools/reach_planner_probe.vcxproj'),'/nologo','/nr:false','/p:Configuration=Release','/p:Platform=x64','/v:minimal','/p:OutDir='+str(out/'bin')+os.sep,'/p:IntDir='+str(out/'obj')+os.sep],capture_output=True,env=dict(os.environ))
        (out/'build.txt').write_bytes(build.stdout+build.stderr)
        if build.returncode:raise RuntimeError('native header probe build failed')
        probe=out/'bin/reach_planner_probe.exe';report['probe_sha256']=sha(probe)
        transport=native_checks(probe);save(out/'transport.json',transport)
        save(out/'hairpin.json',hairpin_checks())
        upper_cache={}
        for name,m in cases:
            row=dict(name=name,model=asdict(m),exact={},greedy={});start=time.perf_counter()
            for mode in MODES:
                for policy,field in (('exact','exact'),('greedy','greedy')):
                    solver=Exact(m,mode,policy);begin=time.perf_counter();result=solver.run();result['wall_seconds']=time.perf_counter()-begin
                    row[field][mode]=result;solver.solve.cache_clear();solver.step.cache_clear()
                assert row['greedy'][mode]['success']<=row['exact'][mode]['success']
            vals={mode:row['exact'][mode]['success'] for mode in MODES}
            assert vals['B1']<=min(vals['B2'],vals['B3'])<=max(vals['B2'],vals['B3'])<=vals['JOINT']
            key=replace(m,delay=0,report_loss=F(0))
            if key not in upper_cache:upper_cache[key]=(full_state_causal(key),hindsight(key))
            row['full_state'],row['hindsight']=upper_cache[key]
            assert vals['JOINT']<=row['full_state']<=row['hindsight']['success']<=1
            if m.delay>=m.horizon or m.report_loss==1:assert len(set(vals.values()))==1
            for field in ('exact','greedy'):
                for val in row[field].values():assert val['expected_ip_bytes']<=m.budget_packets*1272+(m.horizon+1)*64
            row['wall_seconds']=time.perf_counter()-start;report['cases'].append(row)
            print(json.dumps(dict(case=name,values={k:str(v) for k,v in vals.items()},seconds=round(row['wall_seconds'],3))),flush=True)
        known=next(c for c in report['cases'] if c['name']=='known_initial')
        assert len({r['success'] for r in known['exact'].values()})==1
        assert all(sha(repo/f)==h for f,h in hashes.items()),'source changed during study'
        # No native engine mutation or new live trial is necessary for this offline task.
        old=repo/'artifacts/reach_g3_ablation_v5_20260923/verification/study_01/baseline_lock.json'
        lock=json.loads(old.read_text(encoding='utf-8'))
        assert all(sha(repo/f)==h for f,h in lock['source_sha256'].items())
        assert sha(repo/'artifacts/reach_g3_ablation_v5_20260923/bin/Release/GE3.exe')==lock['executable_sha256']
        report['live_engine_unchanged']=True;report['passed']=True
        render(out,report['cases'],transport)
    except Exception as e:report['error']=str(e)
    save(out/'report.json',report)
    if report['passed']:
        save(out/'task_decision.json',dict(passed=True,task='G3-03',completed_at=datetime.now().astimezone().isoformat(),all_completed=22,all_total=38,g3_completed=3,g3_total=5,gates_passed=3,gates_total=7,next='G3-04',scope='finite offline model and native formula diagnostics; not H1/general superiority or live R qualification'))
        files=[p for p in out.rglob('*') if p.is_file() and ('obj' not in p.relative_to(out).parts)]
        save(out/'evidence.json',dict(files={str(p.relative_to(out)):sha(p) for p in files}))
    print(json.dumps(dict(passed=report['passed'],cases=len(report['cases']),error=report.get('error'))))
    return 0 if report['passed'] else 1


def product_grid():
    from itertools import product
    return product((1,2,3),range(6),(0,1,3),(F(0),F(1,2),F(1)))


if __name__=='__main__':raise SystemExit(main())
