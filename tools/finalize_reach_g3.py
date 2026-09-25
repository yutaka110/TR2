"""Read-only audit of G3-01..04; write a NEW G3-05 synthesis and gate record.

Scientific judgments are documented human-readable decisions, not statistical
discoveries made by this script. No original evidence or live source is changed.
"""
import argparse
from collections import Counter
from datetime import datetime
from fractions import Fraction as F
import hashlib
import html
import json
from pathlib import Path
import shutil
import sys
import traceback


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def read(path): return json.loads(path.read_text(encoding='utf-8'))
def rational(value): return F(value['exact'])
def encode(value):
    if isinstance(value, F): return dict(exact=str(value), decimal=float(value))
    raise TypeError(type(value).__name__)
def save(path, value):
    path.write_text(json.dumps(value, default=encode, ensure_ascii=False, indent=2), encoding='utf-8')
def require(condition, message):
    if not condition: raise RuntimeError(message)


def audit(repo):
    inputs, checks = {}, {}
    def record(path):
        path = path.resolve()
        require(path.is_relative_to(repo), 'evidence outside workspace')
        inputs[path.relative_to(repo).as_posix()] = sha(path)
        return path
    def load(path): return read(record(path))
    roots = [repo/'artifacts'/p for p in (
        'reach_g3_baseline_v3_20260922/verification/study_01',
        'reach_g3_ablation_v5_20260923/verification/study_01',
        'reach_g3_exact_20260924/study_03', 'reach_g3_h1_20260924/study_01')]
    for i, root in enumerate(roots, 1):
        task = load(root/'task_decision.json')
        require(task['passed'] and task['task'] == f'G3-0{i}', 'prior task not qualified')
        manifest = load(root/('evidence_audit.json' if i < 3 else 'evidence.json'))
        base = repo if i < 3 else root
        for f, expected in manifest['files'].items():
            require(sha(record(base/f)) == expected, 'frozen evidence mismatch: '+f)
        checks[f'G3-0{i}_evidence_files'] = len(manifest['files'])
        if i >= 3:
            sources = load(root/'source_hashes.json')
            for f, expected in sources.items():
                require(sha(record(repo/f)) == expected and sha(record(root/'source'/f)) == expected,
                        'theoretical source mismatch: '+f)
            checks[f'G3-0{i}_source_files'] = len(sources)

    baseline = load(roots[0]/'qualified_report.json')
    original = load(roots[0]/'report.json')
    disposition = load(roots[0]/'trial_disposition.json')
    require(baseline['passed'] and baseline['complete'] and len(baseline['trials']) == 20,
            'qualified baseline set changed')
    require(sum(not t['passed'] for t in original['trials']) == 1 and not original['passed'],
            'original invalid baseline trial lost')
    require(disposition['invalid_windows'] == 1 and disposition['supplemental_trials'] == 2,
            'supplement disposition changed')
    baseline_success = {m: sum(t['integration']['motion']['outcome'] == 'success'
                              for t in baseline['trials'] if t['phase'] == 'validation' and t['mode'] == m)
                        for m in ('B0', 'B1')}
    require(baseline_success == dict(B0=2, B1=3), 'baseline result mismatch')

    live = load(roots[1]/'report.json')
    aggregate = load(roots[1]/'aggregate.json')
    lock, selection = load(roots[1]/'baseline_lock.json'), load(roots[1]/'selection.json')
    require(live['passed'] and live['complete'] and len(live['trials']) == 42
            and all(t['passed'] for t in live['trials']), 'live study incomplete')
    for f, h in lock['source_sha256'].items():
        require(sha(record(repo/f)) == h, 'live source changed: '+f)
    for root in roots[:2]:
        frozen = load(root/'baseline_lock.json')
        exe = root.parents[1]/'bin/Release/GE3.exe'
        require(sha(record(exe)) == frozen['executable_sha256'], 'historical binary changed')
    require(not selection['holdout_used'], 'selection used holdout')
    validation = [t for t in live['trials'] if t['phase'] == 'validation']
    training = [t for t in live['trials'] if t['phase'] == 'training']
    require(len(training) == 30 and len(validation) == 12, 'phase counts changed')
    rows = []
    for case in sorted({t['case'] for t in validation}):
        group = [t for t in validation if t['case'] == case]
        require(len(group) == 3 and {t['mode'] for t in group} == {'B1', 'B2', 'B3'}, 'unpaired live case')
        rows.extend(dict(case=t['case'], mode=t['mode'], outcome=t['integration']['motion']['outcome'],
                         terminal_seconds=t['integration']['motion']['terminal']['simulation_s'],
                         ip_bytes=t['budget']['summary']['attempted_ip_bytes'], coefficient=t['lambda'])
                    for t in sorted(group, key=lambda t: t['mode']))
    successes = {}
    for mode in ('B1', 'B2', 'B3'):
        ts = [t for t in validation if t['mode'] == mode]
        require(sum(t['mode'] == mode for t in training) == 10, 'unequal training opportunities')
        require(all(t['lambda'] == selection[mode]['lambda'] and t['started_at'] > selection['frozen_at'] for t in ts),
                'selection not frozen before validation')
        outcomes = dict(Counter(t['integration']['motion']['outcome'] for t in ts))
        require(outcomes == aggregate['validation'][mode]['outcomes'], 'outcome aggregate mismatch')
        require(sum(t['budget']['summary']['attempted_ip_bytes'] for t in ts) == aggregate['validation'][mode]['ip_bytes'],
                'cost aggregate mismatch')
        require(sum(t['ablation']['action_changed_against_same_input_B1'] for t in ts) ==
                aggregate['validation'][mode]['counterfactual_changes'], 'decision count mismatch')
        successes[mode] = outcomes.get('success', 0)
    require(successes == dict(B1=2, B2=3, B3=3), 'unexpected live summary')
    require(all(r['outcome'] == 'timeout' for r in rows if r['case'] == 'valid_T2_small_queue'), 'negative case changed')

    exact, transport = load(roots[2]/'report.json'), load(roots[2]/'transport.json')
    require(exact['passed'] and len(exact['cases']) == 165 and transport['passed'], 'exact study incomplete')
    base = next(c for c in exact['cases'] if c['name'] == 'reference_case')
    vals = {m: rational(v['success']) for m, v in base['exact'].items()}
    task_gain = vals['JOINT']-vals['B2']
    require(task_gain == F(1, 256), 'conditional task-information result changed')
    worst = max(transport['grid'], key=lambda c: rational(c['objective_gap']))
    require(rational(worst['objective_gap']) == F(445789, 4014080)
            and transport['scalar_equivalence_checks'] == 15, 'approximation diagnosis changed')
    greedy_gap = max(rational(c['exact'][m]['success'])-rational(c['greedy'][m]['success'])
                     for c in exact['cases'] for m in c['exact'])

    h1 = load(roots[3]/'report.json')
    require(h1['passed'] and len(h1['cases']) == 340 and h1['computed_conditions'] == 316
            and h1['rejected_reservations'] == 24, 'H1 study incomplete')
    paired = next(c for c in h1['cases'] if c['name'] == 'paired_reference')
    region = next(c for c in h1['cases'] if c['name'] == 'region_h3_ip6656_prior1/2_s0')
    require(rational(paired['result']['AGE']['success']) == F(81, 100)
            and rational(paired['result']['REF']['success']) == F(9, 10), 'H1 paired value changed')
    require([x['optimal_actions'] for x in paired['result']['REF']['root']] == [['I'], ['P_REPEAT']],
            'strict optimal-action separation missing')
    require(rational(region['result']['AGE']['success']) < F(19, 20) <= rational(region['result']['REF']['success']),
            '95% feasibility expansion missing')
    zero_controls = [c for c in h1['cases'] if c['result'] is not None and
                     (c['model']['delay'] >= c['model']['horizon'] or rational(c['model']['report_loss']) == 1)]
    require(zero_controls and all(rational(c['gain']) == 0 for c in zero_controls), 'negative notification controls fail')
    for f in ('docs/Reach_RT_G3_Decision.md', 'docs/Reach_RT_G4_Implementation_Plan.md'):
        record(repo/f)
    checks.update(paired_live_conditions=4, independent_reaudits_of_raw_sessions=0,
                  zero_notification_controls=len(zero_controls), live_source_files=len(lock['source_sha256']),
                  historical_executables=2, prior_evidence_unchanged=True)
    return dict(passed=True, checks=checks, inputs=inputs, baseline_successes=baseline_success,
                live_successes=successes, live_validation=rows, live_comparison_count_per_mode=4,
                exact_information_values=vals, conditional_task_information_gain=task_gain,
                native_objective_gap=rational(worst['objective_gap']), greedy_success_gap=greedy_gap,
                h1_paired={m: paired['result'][m]['success'] for m in ('AGE', 'REF')},
                h1_region={m: region['result'][m]['success'] for m in ('AGE', 'REF')},
                scope='frozen artifact integrity and aggregate consistency; no new live trials, raw-log reaudits, statistical or novelty claims')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--name', default='review_01')
    args = p.parse_args()
    require(args.name.replace('_', '').isalnum(), 'unsafe output name')
    repo = Path(__file__).resolve().parents[1]
    out = repo/'artifacts/reach_g3_gate_20260924'/args.name
    out.mkdir(parents=True, exist_ok=False)
    try:
        report = audit(repo)
        save(out/'report.json', report)
        decision = dict(passed=True, gate='G3', task='G3-05', completed_at=datetime.now().astimezone().isoformat(),
            all_completed=24, all_total=38, g3_completed=5, g3_total=5, gates_passed=4, gates_total=7,
            next='G4-01', basis=['strong baselines and qualified information ablations',
                'causal exact models and strict matched H1 counterexample', 'negative results and limits identified',
                'hypothesis-specific decisions and executable G4 requirements documented'],
            hypotheses=dict(H1='conditional support in finite model; live suggestive only',
                H2='not empirically established; continue conditional on decoder information',
                H3='finite-model example; live success-resource curves untested',
                H4='not established; approximation limits identified'),
            all_hypotheses_proven=False, statistical_superiority=False, novelty_established=False,
            R_implemented=False, scope='G3 research-readiness gate only; scientific judgment recorded in decision document')
        save(out/'gate_decision.json', decision)
        save(out/'task_decision.json', decision)
        for f in ('tools/finalize_reach_g3.py', 'docs/Reach_RT_G3_Decision.md', 'docs/Reach_RT_G4_Implementation_Plan.md'):
            dest = out/'source'/f
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(repo/f, dest)
        table = ''.join('<tr>'+''.join('<td>'+html.escape(str(r[k]))+'</td>' for k in
                        ('case', 'mode', 'outcome', 'terminal_seconds', 'ip_bytes'))+'</tr>' for r in report['live_validation'])
        body = '''<!doctype html><html lang="ja"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>Reach-RT G3 / 研究成立判定</title><style>body{font:16px system-ui;background:#edf3f8;color:#19344c;max-width:1200px;margin:32px auto;padding:0 20px}header{background:#152d44;color:white;padding:28px;border-radius:12px}section{background:white;padding:24px;margin:20px 0;border-radius:12px}p{line-height:1.8}table{border-collapse:collapse;width:100%;white-space:nowrap}td,th{padding:10px;border-bottom:1px solid #dae2ec;text-align:left}.scroll{overflow:auto}a{color:#08776e}</style>
<header><h1>G3 通過 / G4へ進む</h1><p>情報の価値と限界を特定。次は有限候補生成と実行接続。</p></header>
<section><h2>判定の範囲</h2><p>H1は限定モデルで条件付き支持。H2、ライブでのH3、H4は未実証として継続します。G3通過は、全仮説の実証・提案Rの優位性・新規性の認定ではありません。</p>
<p>全体24/38項目、G3は5/5、研究ゲート4/7。G4の実装は未着手です。</p></section>
<section><h2>実測の改善例と失敗例を残す</h2><p>G3-02：各条件・各方式1回。正常時に遅くなる例、小キューで全方式未完了の例も含みます。通信費用は共通60秒窓です。時間切れの60秒は完了時間ではありません。</p><div class="scroll"><table><tr><th>条件</th><th>方式</th><th>結果</th><th>終端時刻（秒）</th><th>総IP byte</th></tr>TABLE</table></div></section>
<section><h2>G4で改善する構造</h2><p>現行の重み付けは実効通信費用係数の変更に相当します。G4では、候補ごとに参照回復・後続画像・作業利用までの遷移を予測し、同候補の情報マスク比較と係数調整方式で寄与を検証します。</p>
<p>未出力IDRや存在しないパリティは送信候補にしません。未知条件・古い通知・計算時間超過を記録し、実行前に期限と予算を再検査します。</p></section>
<p><a href="../../../docs/Reach_RT_G3_Decision.md">判定書</a> / <a href="../../../docs/Reach_RT_G4_Implementation_Plan.md">G4実装計画</a> / <a href="report.json">証拠の集約</a> / <a href="gate_decision.json">ゲート判定</a></p></html>'''
        (out/'index.html').write_text(body.replace('TABLE', table), encoding='utf-8')
        # Detect concurrent input edits before certifying this read-only review.
        require(all(sha(repo/f) == h for f, h in report['inputs'].items()), 'input changed during review')
        save(out/'evidence.json', dict(files={p.relative_to(out).as_posix(): sha(p) for p in out.rglob('*') if p.is_file()}))
        print(json.dumps(dict(passed=True, task='G3-05', gate='G3', checks=report['checks'],
                              input_files=len(report['inputs']), next='G4-01')))
        return 0
    except Exception as error:
        save(out/'failure.json', dict(passed=False, error=repr(error)))
        (out/'error.txt').write_text(traceback.format_exc(), encoding='utf-8')
        if (out/'report.json').exists():
            failed_report = read(out/'report.json')
            failed_report.update(passed=False, error=repr(error))
            save(out/'report.json', failed_report)
        # If a late integrity check fails, explicitly revoke any provisional decisions.
        for name in ('gate_decision.json', 'task_decision.json'):
            if (out/name).exists(): save(out/name, dict(passed=False, task='G3-05', error=repr(error)))
        print(json.dumps(dict(passed=False, error=repr(error))))
        return 1


if __name__ == '__main__': sys.exit(main())
