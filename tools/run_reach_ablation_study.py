"""G3-02: equal 5 x 2 tuning, then four three-way paired holdouts, no retries."""
import argparse,html,json,subprocess,sys
from collections import Counter
from pathlib import Path
from run_reach_baseline_study import sha,stamp
from verify_reach_closed_loop import read,save
from verify_reach_command import require
from verify_reach_baseline import baseline_audit
from verify_reach_integration import integration_audit,direction
from verify_reach_budget import budget_audit
from verify_reach_decode import decode_audit
from verify_reach_state import state_audit
from audit_reach_stage_conservation import stage_conservation
from audit_reach_ablation_state import ablation_summary
from generate_reach_trace import write_bundle

CRITICAL=('run_reach_ablation_study.py','run_reach_baseline_study.py','run_reach_g1.py','verify_reach_baseline.py','audit_reach_ablation_state.py','verify_reach_integration.py','verify_reach_budget.py','verify_reach_decode.py','verify_reach_state.py','audit_reach_stage_conservation.py')


def render(out,r):
    sections=[]
    if 'selection' in r:
        cards=[]
        for mode in ('B1','B2','B3'):
            ts=[t for t in r['trials'] if t['phase']=='validation' and t['mode']==mode]
            valid=[t for t in ts if t['passed']];success=sum(t['integration']['motion']['outcome']=='success' for t in valid)
            cards.append(f'<article><h2>{mode}</h2><strong>{success} / {len(ts)}</strong><p>検証条件で作業成功</p><p>固定 λ = {r["selection"][mode]["lambda"]}</p><p>監査合格 {len(valid)} / {len(ts)}</p></article>')
        sections.append('<div class="cards">'+''.join(cards)+'</div>')
    for phase in ('training','validation'):
        cells=[]
        for t in r['trials']:
            if t['phase']!=phase:continue
            motion=t.get('integration',{}).get('motion',{});terminal=motion.get('terminal') or {};a=t.get('ablation',{})
            vals=[t['case'],t['mode'],t['lambda'],motion.get('outcome','未成立'),terminal.get('simulation_s','—'),t.get('budget',{}).get('summary',{}).get('attempted_ip_bytes','—'),a.get('action_changed_against_same_input_B1','—'),'PASS' if t['passed'] else t.get('error','実行中')]
            cells.append('<tr>'+''.join('<td>'+html.escape(str(v))+'</td>' for v in vals)+'</tr>')
        sections.append('<section><h2>'+('調整：全係数' if phase=='training' else '固定後：同じ条件の比較')+'</h2><div class="scroll"><table><tr>'+''.join('<th>'+v+'</th>' for v in ('条件','方式','λ','作業結果','完了秒','IP byte','同一入力でB1から変わった判断','監査'))+'</tr>'+''.join(cells)+'</table></div></section>')
    (out/'index.html').write_text('<!doctype html><html lang="ja"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>Reach-RT G3-02</title><style>body{font:16px system-ui;background:#edf3f8;color:#19344a;max-width:1500px;margin:32px auto;padding:0 24px}section,article{background:white;padding:24px;border-radius:12px;margin:16px 0}.cards{display:flex;gap:20px;flex-wrap:wrap}.cards article{flex:1;min-width:210px}strong{font-size:40px}table{border-collapse:collapse;white-space:nowrap;width:100%}td,th{padding:10px;text-align:left;border-bottom:1px solid #dae4eb}.scroll{overflow:auto}a{color:#087786}</style><h1>Reach-RT / 復号情報と作業情報の寄与</h1><p>B1：回線情報 ／ B2：＋復号情報 ／ B3：＋作業情報</p><p>同じ13候補・通信予算・障害トレース・調整機会。各60秒窓。通知未到着・失効時はB1相当へ戻ります。</p><p>状態：'+('全試行の計測監査合格' if r['passed'] else '実行中または不合格あり')+'</p>'+''.join(sections)+'<p>各条件・各方式1回の初期確認です。統計的優位性や実網性能を保証しません。「B1から変わった判断」は同一入力・同じλでの一段階の再計算であり、別の作業軌跡の予測ではありません。係数を再調整した方式間比較と区別します。</p><p><a href="plan.json">事前計画</a> / <a href="selection.json">全係数と選定</a> / <a href="report.json">全試行・失敗記録</a> / <a href="baseline_lock.json">固定条件</a></p></html>',encoding='utf-8')


def main():
    p=argparse.ArgumentParser();p.add_argument('--name',required=True);p.add_argument('--build-name',default='reach_g3_ablation_v5_20260923');a=p.parse_args()
    for s in (a.name,a.build_name):require(s.replace('_','').replace('-','').isalnum(),'unsafe name')
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/a.build_name;out=root/'verification'/a.name;out.mkdir(parents=True,exist_ok=False)
    exe=sha(root/'bin/Release/GE3.exe');budget=read(repo/'config/reach_rt_g2_budget.json');save(out/'budget.json',budget)
    sources={str(f.relative_to(repo)).replace('\\','/'):sha(f) for folder in ('research','network') for f in (repo/folder).glob('*') if f.suffix in ('.h','.cpp')}
    sources.update({'tools/'+n:sha(repo/'tools'/n) for n in CRITICAL})
    cases=[dict(name='train_T1_mild',task='T1',phase='training',seed=301,queue=65536,rates=[2500000,6000000],loss=10000),dict(name='train_T2_mixed',task='T2',phase='training',seed=302,queue=16384,rates=[1500000,6000000],loss=30000),dict(name='valid_T1_normal',task='T1',phase='validation'),dict(name='valid_T2_normal',task='T2',phase='validation'),dict(name='valid_T1_mixed',task='T1',phase='validation',seed=333,queue=32768,rates=[1500000,6000000],loss=30000),dict(name='valid_T2_small_queue',task='T2',phase='validation',seed=334,queue=8192,rates=[2000000,6000000],loss=20000)]
    for c in cases:
        if 'seed' in c:
            profile={d:dict(queue_ip_bytes=c['queue'] if d=='uplink' else 16384,capacities_bps=c['rates'] if d=='uplink' else [6000000],drop_ppm=c['loss'],base_delay_us=10000,jitter_us=10000) for d in ('uplink','downlink')}
            c['link'],meta=write_bundle(out/(c['name']+'_trace'),seed=c['seed'],horizon_us=60000000,slot_us=100000,profile=profile);c['trace_sha256']=meta['link_sha256']
        else:c['link']=dict(uplink=direction(),downlink=direction())
        save(out/(c['name']+'_link.json'),c['link'])
    lock=dict(version='G3-02-v1',executable_sha256=exe,source_sha256=sources,budget=budget,
              common=dict(budget_link_journal="128 MiB per transport stream; other research logs 32 MiB each; disk writes after measurement",precise_wait=True,process_priority='above_normal',candidate_count=13,fec_groups=[0,2,4,8],repair_caps=[0,1,2],cache_frames=24,idle_probe_us=500000,probe_value=1,probe_lambda=0,codec='H264 640x360 30fps 1500000bps',rsta_all_modes=True,initial_pose=[0,0,0],truth_or_future_input=False),
              masks=dict(B1='transport only',B2='received reference/generation/past decoder wait',B3='received task stage/observation capture/error allowance/deadline'),
              scope='independent retuning plus same-input one-step B1 counterfactual; no combined R policy or interaction effect')
    save(out/'baseline_lock.json',lock);weights=[0,.1,.3,1,3];modes=('B1','B2','B3')
    plan=dict(created_at=stamp(),cases=cases,lambdas=weights,modes=modes,training_trials=30,validation_trials=12,duration_s=60,retries=0,lock_sha256=sha(out/'baseline_lock.json'),
              selection='successes descending; total terminal seconds (failure=60); attempted IP bytes; lambda list order',
              invalid_policy='preserve all trials; stop before selection if training audit invalid; no automatic replacement; validation invalid prevents completion',
              order='rotate B1/B2/B3 by coefficient and condition; reverse lambda order on T2; rotate validation order',
              prior='G3-01 development and seeds disclosed; training 301/302 reused, holdout disorder seeds 333/334 new; normal conditions are regressions')
    save(out/'plan.json',plan);r=dict(passed=False,complete=False,executable_sha256=exe,trials=[])
    def run(c,mode,weight):
        name=c['name']+'_'+mode+'_'+str(weight).replace('.','p');t=dict(id=name,case=c['name'],mode=mode,**{'lambda':weight},phase=c['phase'],passed=False,started_at=stamp());r['trials'].append(t)
        print(json.dumps(dict(starting=name)),flush=True);save(out/'report.json',r)
        try:
            require(sha(root/'bin/Release/GE3.exe')==exe and all(sha(repo/k)==v for k,v in sources.items()),'frozen implementation changed')
            cmd=[sys.executable,str(repo/'tools/run_reach_g1.py'),'--stage','command_udp','--task',c['task'],'--duration','60','--name',a.name+'_'+name,'--build-name',a.build_name,'--budget-config',str(out/'budget.json'),'--state-feedback','--packet-trace','--baseline',mode,'--baseline-lambda',str(weight),'--timeout','95','--process-priority','above_normal','--precise-wait']
            cmd+=['--trace-bundle',str(out/(c['name']+'_trace'))] if 'seed' in c else ['--link-config',str(out/(c['name']+'_link.json'))]
            result=subprocess.run(cmd,cwd=repo,capture_output=True);(out/(name+'.stdout')).write_bytes(result.stdout);(out/(name+'.stderr')).write_bytes(result.stderr)
            inv=root/'runs'/(a.name+'_'+name);session=next((inv/'sessions').iterdir());t['session']=str(session)
            require(result.returncode==0,'native trial failed')
            cfg=read(session/'config.effective.json');require(cfg==read(inv/'requested_config.json') and cfg['ip_budget']==budget and cfg['link_model']==c['link'],'common configuration differs')
            hashes=read(inv/'source_hashes.json');require(read(inv/'launch.json')['executable_sha256']==exe and all(hashes[k]==v for k,v in sources.items()),'snapshot changed')
            t.update(baseline=baseline_audit(session),integration=integration_audit(session,out/name),budget=budget_audit(session),decode=decode_audit(session),state=state_audit(session),ablation=ablation_summary(session))
            conservation=stage_conservation(session);save(out/name/'frame_terminal.json',conservation);t['frame_terminal_counts']=conservation['counts']
            require(not t['integration']['allowance_violations'],'pose allowance exceeded');t['passed']=True
        except Exception as e:t['error']=str(e)
        t['finished_at']=stamp();save(out/'report.json',r);render(out,r)
        print(json.dumps(dict(completed=name,passed=t['passed'],error=t.get('error'),outcome=t.get('integration',{}).get('motion',{}).get('outcome'))),flush=True)
    for ci,c in enumerate(c for c in cases if c['phase']=='training'):
        for wi,w in enumerate(weights if ci==0 else list(reversed(weights))):
            offset=(ci+wi)%3
            for mode in modes[offset:]+modes[:offset]:
                run(c,mode,w)
                if not r['trials'][-1]['passed']:
                    r['error']='training audit invalid; stopped before selection';save(out/'report.json',r);return 1
    if not all(t['passed'] for t in r['trials']):r['error']='training audit invalid; no selection';save(out/'report.json',r);return 1
    selection={}
    for mode in modes:
        scores=[]
        for w in weights:
            ts=[t for t in r['trials'] if t['mode']==mode and t['lambda']==w]
            scores.append(dict(**{'lambda':w},successes=sum(t['integration']['motion']['outcome']=='success' for t in ts),terminal_seconds=sum(t['integration']['motion']['terminal']['simulation_s'] if t['integration']['motion']['outcome']=='success' else 60 for t in ts),attempted_ip_bytes=sum(t['budget']['summary']['attempted_ip_bytes'] for t in ts)))
        best=min(scores,key=lambda s:(-s['successes'],s['terminal_seconds'],s['attempted_ip_bytes'],weights.index(s['lambda'])))
        selection[mode]=dict(**best,training_scores=scores)
    selection.update(frozen_at=stamp(),plan_sha256=sha(out/'plan.json'),holdout_used=False);save(out/'selection.json',selection);r['selection']=selection;save(out/'report.json',r)
    for ci,c in enumerate(c for c in cases if c['phase']=='validation'):
        offset=ci%3
        for mode in modes[offset:]+modes[:offset]:run(c,mode,selection[mode]['lambda'])
    r['complete']=len(r['trials'])==42;r['passed']=r['complete'] and all(t['passed'] for t in r['trials'])
    r['outcomes']=dict(Counter(t.get('integration',{}).get('motion',{}).get('outcome','invalid') for t in r['trials']));save(out/'report.json',r);render(out,r)
    return 0 if r['passed'] else 1

if __name__=='__main__':raise SystemExit(main())
