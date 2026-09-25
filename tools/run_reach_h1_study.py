"""G3-04 reproducible counterexample, negative controls and finite feasibility grid."""
import argparse
from dataclasses import asdict, replace
from datetime import datetime
from fractions import Fraction as F
import hashlib
import html
from itertools import product
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import traceback

from reach_h1_model import H1Model, evaluate, histories

FILES = ('tools/reach_h1_model.py', 'tools/test_reach_h1.py', 'tools/run_reach_h1_study.py',
         'tools/verify_reach_h1_artifacts.py', 'tools/reach_exact_model.py')
TARGET = F(19, 20)


def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()


def encode(x):
    if isinstance(x, F): return dict(exact=str(x), decimal=float(x))
    raise TypeError(type(x).__name__)


def save(path, x):
    path.write_text(json.dumps(x, default=encode, ensure_ascii=False, indent=2), encoding='utf-8')


def plan():
    m = H1Model()
    cases = [dict(name='paired_reference', group='pair', model=asdict(m))]
    for p, prior, b in product((F(0), F(1, 4), F(1, 2), F(3, 4), F(9, 10), F(99, 100), F(1)),
                               (F(0), F(1, 4), F(1, 2), F(3, 4), F(1)), range(3)):
        cases.append(dict(name=f'one_p{p}_prior{prior}_b{b}', group='one_slot',
                          model=asdict(replace(m, packet_success=p, ref_prior=prior, budget_packets=b))))
    for delay, q, reset in product((0, 1, 3), (F(0), F(1, 2), F(1)), (F(0), F(1, 4))):
        cases.append(dict(name=f'feedback_d{delay}_q{q}_reset{reset}', group='feedback',
                          model=asdict(replace(m, horizon=3, budget_packets=4, initial_stage=0,
                                              delay=delay, report_loss=q, reset=reset))))
    # Fix TOTAL IP budget across deadlines; reserve all report opportunities first.
    for h, budget, prior, stage in product(range(1, 5), (128, 256, 1536, 2816, 4096, 5376, 6656, 8192, 10496),
                                          (F(0), F(1, 2), F(1)), (0, 1)):
        reserve = (h+1)*64
        cases.append(dict(name=f'region_h{h}_ip{budget}_prior{prior}_s{stage}', group='region',
                          total_ip_budget=budget, notification_reserve=reserve,
                          reservation_fits=budget >= reserve,
                          model=asdict(replace(m, horizon=h, ref_prior=prior, initial_stage=stage,
                                              budget_packets=max(0, (budget-reserve)//1272)))))
    return cases


def calculate(cases):
    rows = []
    for case in cases:
        row = dict(case)
        if not case.get('reservation_fits', True):
            row.update(result=None, gain=None, feasible=None)
        else:
            m = H1Model(**case['model'])
            result = evaluate(m)
            row.update(result=result, gain=result['REF']['success']-result['AGE']['success'],
                       feasible={mode: v['success'] >= TARGET for mode, v in result.items()})
            if m.delay >= m.horizon or m.report_loss == 1 or m.budget_packets == 0:
                assert row['gain'] == 0
            if m.horizon == 1:
                p, z = m.packet_success, m.ref_prior
                base = F(0) if m.budget_packets == 0 else z*p
                informed = base
                if m.budget_packets >= 2:
                    base = max(p*p, z*(2*p-p*p))
                    informed = (1-z)*p*p+z*(2*p-p*p)
                if m.initial_stage == 0:
                    base = informed = F(0)  # at most one usable frame per slot
                if m.delay > 0: informed = base
                expected = (1-m.report_loss)*informed+m.report_loss*base
                assert result['AGE']['success'] == base
                assert result['REF']['success'] == expected
        rows.append(row)
    pair = rows[0]['result']
    assert pair['AGE']['success'] == F(81, 100) and pair['REF']['success'] == F(9, 10)
    assert [r['optimal_actions'] for r in pair['REF']['root']] == [['I'], ['P_REPEAT']]
    return rows


def boundary_table(rows):
    result = []
    for h, prior, stage, mode in product(range(1, 5), (F(0), F(1, 2), F(1)), (0, 1), ('AGE', 'REF')):
        selected = [r for r in rows if r['group'] == 'region' and r['result'] is not None
                    and r['model']['horizon'] == h and r['model']['ref_prior'] == prior
                    and r['model']['initial_stage'] == stage and r['feasible'][mode]]
        result.append(dict(horizon=h, ref_prior=prior, initial_stage=stage, mode=mode,
                           minimum_tested_total_ip=min((r['total_ip_budget'] for r in selected), default=None)))
    return result


def render(out, rows, boundaries):
    def pct(v): return f'{float(v)*100:.4f}%'
    table = []
    for row in rows:
        m, result = row['model'], row['result']
        if result is None:
            values = ['予約費用超過', '—', '—', '—']
        else:
            values = [pct(result[k]['success'])+f' ({result[k]["success"]})' for k in ('AGE', 'REF')]
            values += [f'{float(row["gain"])*100:.4f} pt',
                       ' / '.join(k+(' ○' if row['feasible'][k] else ' ×') for k in ('AGE', 'REF'))]
        cells = [row['name'], m['horizon'], row.get('total_ip_budget', m['budget_packets']*1272+(m['horizon']+1)*64),
                 m['budget_packets'], m['ref_prior'], m['initial_stage']] + values
        table.append('<tr>'+''.join('<td>'+html.escape(str(x))+'</td>' for x in cells)+'</tr>')
    paired = rows[0]['result']['REF']['root']
    qrows = []
    for index, record in enumerate(paired):
        qrows.append('<tr><th>'+('参照なし' if index == 0 else '参照あり')+'</th>'+''.join(
            '<td>'+pct(a['success'])+'</td>' for a in record['alternatives'])+'</tr>')
    region = []
    for prior in (F(0), F(1, 2), F(1)):
        cells = []
        for h in range(1, 5):
            found = [b for b in boundaries if b['ref_prior'] == prior and b['horizon'] == h
                     and b['initial_stage'] == 1 and b['mode'] == 'REF'][0]
            value = found['minimum_tested_total_ip']
            cells.append('<td>'+('該当なし' if value is None else f'{value:,} byte')+'</td>')
        region.append(f'<tr><th>参照保持の事前確率 {prior}</th>'+''.join(cells)+'</tr>')
    matrix = []
    for budget in (128, 256, 1536, 2816, 4096, 5376, 6656, 8192, 10496):
        cells = []
        for h in range(1, 5):
            row = next(r for r in rows if r['name'] == f'region_h{h}_ip{budget}_prior1/2_s0')
            if row['result'] is None:
                label, color = '予約費用超過', '#e6e8eb'
            elif row['feasible']['AGE']:
                label, color = '両方式 ≥95%', '#ceeae0'
            elif row['feasible']['REF']:
                label, color = 'REFのみ ≥95%', '#fff0b5'
            else:
                label, color = '両方式 <95%', '#f4dfe0'
            if row['result']:
                label += '<br><small>'+pct(row['result']['AGE']['success'])+' / '+pct(row['result']['REF']['success'])+'</small>'
            cells.append(f'<td style="background:{color}">{label}</td>')
        matrix.append(f'<tr><th>{budget:,} byte</th>'+''.join(cells)+'</tr>')
    body = '''<!doctype html><html lang="ja"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>Reach-RT / H1の反例と成立領域</title><style>
body{font:16px system-ui;color:#193149;background:#eef3f8;max-width:1400px;margin:30px auto;padding:0 22px}
header{background:#132b43;color:white;padding:28px;border-radius:14px}section,article{background:white;padding:24px;border-radius:12px;margin:18px 0}
.cards{display:flex;gap:18px;flex-wrap:wrap}.cards article{flex:1;min-width:200px}strong.metric{font-size:38px;color:#097969}
.scroll{overflow:auto}table{border-collapse:collapse;width:100%;white-space:nowrap}th,td{padding:12px;border-bottom:1px solid #d9e3ed;text-align:right}th:first-child,td:first-child{text-align:left}
small{color:#586d80}input{font:inherit;padding:10px;width:min(80%,550px)}a{color:#087885}p{line-height:1.75}</style>
<header><h1>Reach-RT / H1の反例と成立領域</h1><p>画像の古さが同じでも、次の画像を復号できるとは限らない。</p></header>
<p>G3-04の有限モデルによる厳密計算。実アプリの成功率・統計的な優位性・提案方式Rの実測ではありません。</p>
<div class="cards"><article>鮮度・通信・作業情報での最適解<br><strong class="metric">81%</strong><p>両状態でI画像を選ぶ</p></article>
<article>到着した参照情報も使う最適解<br><strong class="metric">90%</strong><p>参照なしはI / ありはP反復</p></article>
<article>情報の価値<br><strong class="metric">+9 pt</strong><p>同じ候補・同じ2パケット予算</p></article></div>
<section><h2>同じ条件、違う最適判断</h2><p>同じI画像を受信し、制御側はその画像を保持。片方だけ復号器の参照バッファを消去します。
損失・遅延履歴、AoI=2、作業段階、将来の通信確率は共通です。残り1スロット、各パケットの独立到達確率90%、両状態の事前確率は各1/2。
通知は判断前に到着し、両方式とも同じ費用を支払います。隠れた状態の真値を直接判断へ渡しません。</p>
<div class="scroll"><table><tr><th>状態別の成功確率</th><th>WAIT</th><th>P</th><th>P反復</th><th>I</th></tr>QROWS</table></div>
<p>この例は「通信統計とAoIだけで常に十分」という主張の反例です。H1自体を否定する反例ではありません。</p></section>
<section><h2>追加情報で95%成立に変わる点</h2><p>残り2段階、参照保持の事前確率1/2。3スロット・総IP予算6,656 byteでは、AGE最適94.041%に対しREF最適96.39%。
試験した中の最小予算はAGEの8,192 byteに対しREFは6,656 byteです。到達確率90%、即時通知、通知欠落・追加flushなし。</p>
<div class="scroll"><table><tr><th>固定総IP予算</th><th>期限1 slot</th><th>期限2 slots</th><th>期限3 slots</th><th>期限4 slots</th></tr>MATRIX</table></div>
<p>セルの数値はAGE / REF。期限を延ばすと固定通知予約も増え、使える映像パケットが減る境目があります。この固定総予算の表では、期限を延ばしても必ず改善するとは限りません。</p></section>
<section><h2>95%成立に必要な、試験した中で最小の総IP予算</h2>
<p>残り1段階・到達確率90%・即時通知・通知欠落なし・追加flushなし・REF方式。総予算は映像と固定通知予約64×(H+1) byteを含みます。
事前確率0/1は初期参照が既知、1/2は通知で解消する不確かさです。</p><div class="scroll"><table><tr><th>初期状態</th><th>期限1 slot</th><th>期限2 slots</th><th>期限3 slots</th><th>期限4 slots</th></tr>REGION</table></div>
<p>「該当なし」は試験範囲内に95%以上の点がないこと。連続的な最小予算や未試験点を断定する補間ではありません。95%は既知確率モデルの目標値で、信頼区間ではありません。</p></section>
<section><h2>価値が消える条件も保存</h2><p>通知が期限までに届かない、全通知欠落、予算ゼロ、通信が必ず成功または必ず失敗する最小例では追加情報の価値は0です。
初期参照が既知で追加flushがなく、ACK・AoIから参照を推測できる条件も差がありません。</p>
<p>このflushは受信側ローカル事象を抽象化した仮定です。発生頻度は実機から推定していません。作業は2回の利用可能画像で進む抽象モデルで、運動・認識誤差・制動・画像の有効期限は含みません。</p></section>
<section><h2>全条件と正確な分数</h2><p>AGEにも到着した画像の撮影時刻・ACK・作業段階と全観測履歴を与えます。REFはそれに参照保持を追加します。ライブB1/B2の成績とは異なります。</p>
<input id="filter" aria-label="条件検索" placeholder="region_h2、feedback、prior1/2 などで検索"><p id="count"></p><div class="scroll"><table><thead><tr>
<th>条件</th><th>期限</th><th>総IP予算</th><th>映像packet</th><th>参照prior</th><th>完了段階</th><th>AGE最適</th><th>REF最適</th><th>差</th><th>95%判定</th>
</tr></thead><tbody>TABLE</tbody></table></div></section>
<p><a href="report.json">全計算・全初手の価値</a> / <a href="plan.json">計算前の条件</a> / <a href="histories.json">対応する履歴対</a> /
<a href="boundaries.json">95%境界</a> / <a href="tests.txt">独立検算</a> / <a href="task_decision.json">完了判定</a> / <a href="evidence.json">証拠ハッシュ</a></p>
<script>const rows=Array.from(document.querySelectorAll('tbody tr'));function filter(){let n=0;const s=document.getElementById('filter').value.toLowerCase();rows.forEach(r=>{r.hidden=!r.textContent.toLowerCase().includes(s);if(!r.hidden)n++});document.getElementById('count').textContent=n+' / '+rows.length+' 条件';}document.getElementById('filter').addEventListener('input',filter);filter();</script></html>'''
    (out/'index.html').write_text(body.replace('QROWS', ''.join(qrows)).replace('REGION', ''.join(region))
                                .replace('MATRIX', ''.join(matrix)).replace('TABLE', ''.join(table)), encoding='utf-8')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--name', default='study_01')
    args = parser.parse_args()
    if not args.name.replace('_', '').isalnum(): raise ValueError('invalid study name')
    repo = Path(__file__).resolve().parents[1]
    out = repo/'artifacts/reach_g3_h1_20260924'/args.name
    out.mkdir(parents=True, exist_ok=False)
    hashes = {f: sha(repo/f) for f in FILES}
    for f in FILES:
        dest = out/'source'/f
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(repo/f, dest)
    save(out/'source_hashes.json', hashes)
    cases = plan()
    save(out/'plan.json', dict(version='G3-04-v1', created_at=datetime.now().astimezone().isoformat(),
                              target=TARGET, cases=cases, source_hashes=hashes,
                              claim='finite decoder-buffer-loss model only; not empirical superiority'))
    report = dict(passed=False, python=sys.version, platform=platform.platform())
    try:
        tests = subprocess.run([sys.executable, str(repo/'tools/test_reach_h1.py')], capture_output=True)
        (out/'tests.txt').write_bytes(tests.stdout+tests.stderr)
        if tests.returncode: raise RuntimeError('independent checks failed')
        rows = calculate(cases)
        print(f'Calculated {len(rows)} conditions; checking repeat execution.', flush=True)
        assert rows == calculate(cases), 'repeat execution mismatch'
        boundaries = boundary_table(rows)
        save(out/'histories.json', histories())
        save(out/'boundaries.json', boundaries)
        old = repo/'artifacts/reach_g3_ablation_v5_20260923/verification/study_01/baseline_lock.json'
        lock = json.loads(old.read_text(encoding='utf-8'))
        assert all(sha(repo/f) == h for f, h in lock['source_sha256'].items()), 'live source changed'
        assert sha(repo/'artifacts/reach_g3_ablation_v5_20260923/bin/Release/GE3.exe') == lock['executable_sha256']
        prior = repo/'artifacts/reach_g3_exact_20260924/study_03'
        frozen = json.loads((prior/'source_hashes.json').read_text(encoding='utf-8'))
        assert all(sha(repo/f) == h for f, h in frozen.items()), 'G3-03 sources changed'
        prior_evidence = json.loads((prior/'evidence.json').read_text(encoding='utf-8'))['files']
        assert all(sha(prior/f) == h for f, h in prior_evidence.items()), 'G3-03 evidence changed'
        assert all(sha(repo/f) == h for f, h in hashes.items()), 'study source changed'
        report.update(passed=True, target=TARGET, cases=rows, reproduced_conditions=len(rows),
                      computed_conditions=sum(r['result'] is not None for r in rows),
                      rejected_reservations=sum(r['result'] is None for r in rows),
                      max_nodes=max(v['nodes'] for r in rows if r['result'] for v in r['result'].values()),
                      independent_comparisons=50, live_engine_unchanged=True,
                      g3_03_sources_checked=len(frozen), g3_03_evidence_checked=len(prior_evidence),
                      prior_evidence_sha256=sha(prior/'evidence.json'))
        render(out, rows, boundaries)
    except Exception as error:
        report['passed'] = False
        report['error'] = repr(error)
        (out/'error.txt').write_text(traceback.format_exc(), encoding='utf-8')
    save(out/'report.json', report)
    if report['passed']:
        save(out/'task_decision.json', dict(task='G3-04', passed=True, all_completed=23, all_total=38,
             g3_completed=4, g3_total=5, gates_passed=3, gates_total=7, next='G3-05',
             scope='strict paired counterexample and finite 95% region; H1 conditional support, no live R claim'))
        save(out/'evidence.json', dict(files={str(p.relative_to(out)): sha(p)
                                            for p in out.rglob('*') if p.is_file()}))
    print(json.dumps({k: v for k, v in report.items() if k != 'cases'}, default=encode))
    return 0 if report['passed'] else 1


if __name__ == '__main__': raise SystemExit(main())
