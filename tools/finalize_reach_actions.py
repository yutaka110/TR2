"""Freeze G4-01 wiring evidence; never infer research superiority from smoke tests."""
import argparse,hashlib,html,json,re,shutil,sys
from collections import Counter
from pathlib import Path
from verify_reach_actions import action_audit,fault_checks
from verify_reach_baseline import baseline_audit
from verify_reach_budget import budget_audit
from verify_reach_decode import decode_audit
from verify_reach_state import state_audit
from verify_reach_integration import integration_audit
from verify_reach_closed_loop import read,save
from verify_reach_command import require

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()

def main():
    p=argparse.ArgumentParser();p.add_argument('--name',required=True);p.add_argument('--build-name',default='reach_g4_actions_20260924');a=p.parse_args()
    for value in (a.name,a.build_name):require(value.replace('_','').isalnum(),'unsafe name')
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/a.build_name;out=root/'verification'/a.name
    out.mkdir(parents=True,exist_ok=False);inputs={}
    def record(path):
        path=path.resolve();require(path.is_relative_to(repo),'outside workspace')
        inputs[path.relative_to(repo).as_posix()]=sha(path);return path
    def load(path):return read(record(path))
    reports={key:load(root/'verification'/name/'report.json') for key,name in dict(build='build_02',acceptance='acceptance_02',units='regression_units_01',ablation='ablation_regression_01',legacy='legacy_regression_01').items()}
    require(all(r['passed'] for r in reports.values()),'prerequisite failure')
    build=reports['build'];exe=record(root/'bin/Release/GE3.exe')
    require(sha(exe)==build['executable_sha256']==reports['legacy']['executable_sha256'],'executable changed')
    for f,h in build['source_hashes'].items():require(sha(record(repo/f))==h,'native source changed: '+f)
    unit_result=load(root/'verification/build_02/command_1.txt')
    require(unit_result['passed'] and unit_result['checks']==684,'unit count')
    units=sum(int(re.search(r'PASS (\d+)',t['output'])[1]) for t in reports['units']['tests'])
    require(units==2000,'regression unit count')
    require(len(reports['acceptance']['cases'])==7 and len(reports['ablation']['cases'])==9,'incomplete matrix')
    b0session=next((root/'runs/b0_regression_01/sessions').iterdir())
    b0=dict(passed=True,session=str(b0session),baseline=baseline_audit(b0session),budget=budget_audit(b0session),decode=decode_audit(b0session),state=state_audit(b0session),integration=integration_audit(b0session,out/'b0'))
    save(out/'b0_report.json',b0)
    trials=reports['acceptance']['cases']+reports['ablation']['cases']+[b0]
    for trial in trials:
        require(trial['passed'],'failed trial');session=Path(trial['session']);run=session.parent.parent
        launch=load(run/'launch.json');require(launch['executable_sha256']==build['executable_sha256'] and launch['exit_code']==0,'trial executable/failure')
        hashes=load(run/'source_hashes.json')
        for f,h in build['source_hashes'].items():
            require(hashes[f]==h and sha(record(run/'research_source'/f))==h,'trial native snapshot changed: '+f)
        for path in session.iterdir():
            if path.is_file():record(path)
        record(run/'requested_config.json')
        if trial in reports['acceptance']['cases']:
            require(action_audit(session)==trial['action'],'action re-audit changed')
    require(len(fault_checks(Path(reports['acceptance']['cases'][0]['session'])))==6,'fault checks')
    # Preserve the historical G3 outputs. Current native code intentionally differs.
    old=repo/'artifacts/reach_g3_gate_20260924/review_01'
    manifest=load(old/'evidence.json')
    for f,h in manifest['files'].items():require(sha(record(old/f))==h,'historical G3 output changed')
    totals=Counter();selected=Counter();classes=Counter();statuses=Counter()
    for case in reports['acceptance']['cases']:
        action=case['action'];selected.update(action['selected']);classes.update(action['sent_classes']);statuses.update(action['wire_status'])
        for key in ('decisions','candidate_rows','xor_packets_checked','idr_requests','idr_outputs'):totals[key]+=action[key]
    aggregate=dict(passed=True,totals=dict(totals),selected=dict(selected),sent_classes=dict(classes),wire_status=dict(statuses),new_unit_checks=684,regression_unit_checks=units,acceptance_cases=7,baseline_regression_cases=10,legacy_displayed_frames=reports['legacy']['displayed_frames'],audit_mutations_rejected=6,historical_G3_output_files=len(manifest['files']))
    save(out/'aggregate.json',aggregate)
    decision=dict(task='G4-01',passed=True,completed_items=25,total_items=38,G4_completed=1,G4_total=5,passed_gates=4,total_gates=7,next='G4-02',scope='finite candidates and real execution wiring',research_superiority=False,periodic_scheduler_implemented=False,full_18_pose_rerun=False)
    save(out/'task_decision.json',decision)
    paths=set(build['source_hashes'])|{'GE3.vcxproj','tools/run_reach_g1.py','tools/build_reach_actions.py','tools/verify_reach_actions.py','tools/finalize_reach_actions.py','tools/test_reach_actions.cpp','tools/reach_actions_tests.vcxproj','docs/Reach_RT_G4_Actions.md'}
    paths.update(p.relative_to(repo).as_posix() for pattern in ('verify_reach_*.py','audit_reach_*.py') for p in (repo/'tools').glob(pattern))
    source={}
    for f in sorted(paths):
        path=record(repo/f);target=out/'source'/f;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(path,target);source[f]=sha(path)
    save(out/'source_hashes.json',source);save(out/'inputs.json',inputs)
    tr=''.join('<tr><td>'+html.escape(c['name'])+'</td><td>PASS</td><td>'+str(c['action']['decisions'])+'</td><td>'+str(c['action']['sent_classes'].get('retransmission',0))+'</td><td>'+str(c['action']['xor_packets_checked'])+'</td><td>'+str(c['action']['idr_outputs'])+'</td></tr>' for c in reports['acceptance']['cases'])
    page='''<!doctype html><html lang="ja"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Reach-RT G4-01</title><style>body{margin:0;background:#eef3f8;color:#172e49;font:17px/1.7 system-ui,sans-serif}main{max-width:1060px;margin:auto;padding:36px}header{background:#142c47;color:white;padding:28px;border-radius:16px}h1{margin:0}section{background:white;padding:24px;margin:20px 0;border-radius:14px}.cards{display:flex;gap:18px;flex-wrap:wrap}.cards div{flex:1;background:#e8f4ee;padding:18px;min-width:150px}strong{font-size:26px}table{width:100%;border-collapse:collapse}td,th{text-align:left;padding:9px;border-bottom:1px solid #dbe3ed}a{color:#145bb5}code{overflow-wrap:anywhere}</style><main><header><div>REACH-RT / RESEARCH EVIDENCE</div><h1>G4-01 有限候補生成と実行接続</h1><p>5種類・7候補 → 実送信の再検査 → 共通IP予算 → 実UDP</p></header><section class="cards"><div>今回の工程<br><strong>合格</strong></div><div>全体の項目<br><strong>25 / 38</strong></div><div>G4の項目<br><strong>1 / 5</strong></div><div>研究ゲート<br><strong>4 / 7</strong></div></section><section><h2>実行できる操作を、記録で説明する</h2><p>再送・新規画像・実XOR付き画像・IDR生成要求・見送りを接続。要求しただけのIDRや、送信待ちで古くなったパケットを送信成功に数えません。</p><p>単体684検査、既存単体2,000検査、実UDP 7条件、B0〜B3の10条件、従来モードが合格。監査への6種類の改変も検出。</p><table><thead><tr><th>条件</th><th>監査</th><th>判断数</th><th>実再送</th><th>実XOR照合</th><th>IDR実出力</th></tr></thead><tbody>'''+tr+'''</tbody></table></section><section><h2>判定の範囲と次工程</h2><p>これは短時間の実行接続検査です。作業成功率・方式の優位性・G4全体の合格は未判定。現在の選択は接続確認用の固定規則で、20 ms周期の最適化処理は未実装です。</p><p>次はG4-02：到着・参照回復・作業価値の予測と校正。</p><p><a href="aggregate.json">集約JSON</a> / <a href="task_decision.json">完了判定</a> / <a href="source/docs/Reach_RT_G4_Actions.md">実装説明</a> / <a href="inputs.json">入力ハッシュ</a></p></section></main></html>'''
    (out/'index.html').write_text(page,encoding='utf-8')
    save(out/'evidence.json',dict(files={p.relative_to(out).as_posix():sha(p) for p in sorted(out.rglob('*')) if p.is_file()}))
    print(json.dumps(aggregate,ensure_ascii=False));return 0
if __name__=='__main__':sys.exit(main())
