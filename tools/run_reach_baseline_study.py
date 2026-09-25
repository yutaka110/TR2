"""G3-01: preregister two training traces, tune five lambdas, freeze, validate.

Every trial uses real C++ H264/UDP, serial execution, the full 60 s window and
identical IP ceilings. No reruns, omitted failures or holdout tuning.
"""
import argparse
from collections import Counter
from datetime import datetime
import hashlib
import html
import json
from pathlib import Path
import subprocess
import sys
from verify_reach_closed_loop import read, save
from verify_reach_command import require
from verify_reach_integration import integration_audit, direction
from verify_reach_budget import budget_audit
from verify_reach_decode import decode_audit
from verify_reach_state import state_audit
from verify_reach_baseline import baseline_audit
from audit_reach_stage_conservation import stage_conservation
from generate_reach_trace import write_bundle


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def stamp():return datetime.now().astimezone().isoformat()


def render(out, report):
    body=[]
    for phase in ('training','validation'):
        entries=[r for r in report['trials'] if r['phase']==phase]
        lines=[]
        for r in entries:
            motion=r.get('integration',{}).get('motion',{});terminal=motion.get('terminal') or {}
            baseline=r.get('baseline',{});budget=r.get('budget',{}).get('summary',{})
            lines.append('<tr>'+''.join('<td>'+html.escape(str(x))+'</td>' for x in (
                r['case'],r['mode'],r['lambda'],motion.get('outcome','--'),terminal.get('simulation_s','--'),
                f"{budget.get('attempted_ip_bytes',0):,}",r.get('decode',{}).get('decoded','--'),
                baseline.get('decision_us_p95','--'),'PASS' if r['passed'] else r.get('error','running')))+f"<td><a href='{r['id']}/frame_chain.csv'>画像</a> / <a href='{r['id']}/actuation_chain.csv'>実動作</a></td></tr>")
        body.append('<section><h2>'+('調整用・全候補' if phase=='training' else '調整後・別条件の対応比較')+'</h2><div class="scroll"><table><thead><tr>'+''.join('<th>'+s+'</th>' for s in ('条件','方式','λ','作業結果','完了秒','通信 IP byte','復号画像','判断p95 μs','監査','経路CSV'))+'</tr></thead><tbody>'+''.join(lines)+'</tbody></table></div></section>')
    selection=report.get('selection',{})
    (out/'index.html').write_text('<!doctype html><html lang="ja"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>Reach-RT G3-01 比較方式</title><style>body{font:16px system-ui;color:#142d45;background:#eef3f8;margin:36px auto;max-width:1450px;padding:0 24px}h1{font-size:30px}section{background:white;padding:24px;margin:24px 0;border-radius:12px}.scroll{overflow:auto}table{border-collapse:collapse;width:100%;white-space:nowrap}th,td{text-align:left;padding:10px;border-bottom:1px solid #dee6ef}th{color:#456078}a{color:#087b92}.status{background:#142d45;color:white;padding:24px;border-radius:12px}</style><h1>Reach-RT / 同じ通信予算で、回復方式を比較</h1><div class="status">G3-01 '+('完了' if report.get('passed') else '計測・判定中')+' ・ B1 固定 λ = '+str(selection.get('lambda','選定前'))+'</div><p>各60秒・実H.264・双方向UDP。映像、再送、FEC、ACK、指令、状態通知を含むIP費用。成功後も60秒まで通信を計上します。</p><p>同じ外生障害トレースと上限を使用。実際の送信数・損失数・通信量は方式の判断により変わります。下表は有限条件の初期調整・動作検証であり、統計的優位性や実網性能の証明ではありません。</p>'+''.join(body)+'<p><a href="plan.json">事前計画</a> / <a href="selection.json">調整結果・固定</a> / <a href="report.json">全監査</a> / <a href="baseline_lock.json">比較条件の固定</a></p></html>',encoding='utf-8')


def main():
    p=argparse.ArgumentParser();p.add_argument('--name',required=True);p.add_argument('--build-name',default='reach_g3_baseline_v3_20260922');args=p.parse_args()
    for x in (args.name,args.build_name):require(x.replace('_','').replace('-','').isalnum(),'unsafe name')
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/args.build_name;out=root/'verification'/args.name;out.mkdir(parents=True,exist_ok=False)
    executable=sha(root/'bin/Release/GE3.exe');budget=read(repo/'config/reach_rt_g2_budget.json');save(out/'budget.json',budget)
    native={str(f.relative_to(repo)).replace('\\','/'):sha(f) for folder in ('research','network') for f in (repo/folder).glob('*') if f.suffix in ('.cpp','.h')}
    cases=[dict(name='train_T1_mild',task='T1',phase='training',seed=301,queue=65536,rates=[2500000,6000000],loss=10000),
           dict(name='train_T2_mixed',task='T2',phase='training',seed=302,queue=16384,rates=[1500000,6000000],loss=30000),
           dict(name='valid_T1_normal',task='T1',phase='validation'),dict(name='valid_T2_normal',task='T2',phase='validation'),
           dict(name='valid_T1_mixed',task='T1',phase='validation',seed=323,queue=32768,rates=[1500000,6000000],loss=30000),
           dict(name='valid_T2_small_queue',task='T2',phase='validation',seed=324,queue=8192,rates=[2000000,6000000],loss=20000)]
    for c in cases:
        if 'seed' in c:
            profile={d:dict(queue_ip_bytes=c['queue'] if d=='uplink' else 16384,capacities_bps=c['rates'] if d=='uplink' else [6000000],drop_ppm=c['loss'],base_delay_us=10000,jitter_us=10000) for d in ('uplink','downlink')}
            link,meta=write_bundle(out/(c['name']+'_trace'),seed=c['seed'],horizon_us=60000000,slot_us=100000,profile=profile)
            c['trace_sha256']=meta['link_sha256'];c['link']=link
        else:c['link']=dict(uplink=direction(),downlink=direction())
        save(out/(c['name']+'_link.json'),c['link'])
    lock=dict(version='G3-01-v3',created_at=stamp(),executable_sha256=executable,native_source_sha256=native,budget=budget,
              common=dict(codec='H264 auto 640x360 30fps 1500000bps',cache_frames=24,deadline_us=200000,pacing=True,
                          pacing_ceiling_bps=3500000,ping_period_us=250000,keyframe_request_cooldown_us=500000,forced_idr_input=45,
                          rsta_sent_by_both=True,rsta_used_by_baseline=False,controller='unchanged image-only',initial_pose=[0,0,0],
                          queue_input='live queued UDP payload bytes; historical residence time excluded'),
              B0=dict(profile='existing RNVP adaptive FEC + deadline repair; research transport adapter',lambda_unused=.1,
                      remote_expiry_and_recovery_counters=0,planner=False,repair='existing shared filters; gate allows'),
              B1=dict(candidate_count=13,groups=[0,2,4,8],repair_request_caps=[0,1,2],loss_ewma_gain=.05,burst_inflation=.25,rtt_ewma_gain=.125,guard_us=8000,
                      idle_probe_us=500000,probe_effective_lambda=0,probe_uses_known_budget_ceiling=True,probe_never_bypasses_deadline_or_IP_budget=True),
              limitations=['own RNVP/XOR reference, not Hairpin/Tooth reproduction','single-PC time-varying emulated network',
                           'BWE/pacer inputs causally acquired but not numerically replayed by independent audit','G2 diagnostic settings are not B0'])
    save(out/'baseline_lock.json',lock)
    lambdas=[0,.1,.3,1,3]
    plan=dict(created_at=stamp(),cases=cases,lambdas=lambdas,duration_s=60,lock_sha256=sha(out/'baseline_lock.json'),
              predecessor='v1 queue semantics fixed in v2; v2 training exposed stale-loss/defer loop; retain development cohorts, add bounded idle probing, use new validation seeds 323/324',
              selection='lexicographic: most training task successes, least total terminal time (failure=60), least total attempted IP bytes, listed lambda order',
              planned_training_trials=12,planned_validation_trials=8,retries=0,validation_pair_order='alternating B0/B1 then B1/B0',
              scope='initial tuning and qualification; not efficacy statistics; future B2/B3/R receive same five-lambda/two-trace tuning budget')
    save(out/'plan.json',plan);report=dict(passed=False,complete=False,executable_sha256=executable,trials=[])
    def run(c,mode,weight,phase):
        label=str(weight).replace('.','p');name=c['name']+'_'+mode+'_'+label
        item=dict(id=name,case=c['name'],mode=mode,**{'lambda':weight},phase=phase,passed=False,started_at=stamp());report['trials'].append(item)
        print(json.dumps(dict(starting=name,phase=phase)),flush=True);save(out/'report.json',report)
        try:
            require(sha(root/'bin/Release/GE3.exe')==executable,'executable changed')
            command=[sys.executable,str(repo/'tools/run_reach_g1.py'),'--stage','command_udp','--task',c['task'],'--duration','60',
                     '--name',args.name+'_'+name,'--build-name',args.build_name,'--budget-config',str(out/'budget.json'),
                     '--state-feedback','--packet-trace','--baseline',mode,'--baseline-lambda',str(weight),'--timeout','95']
            command+=['--trace-bundle',str(out/(c['name']+'_trace'))] if 'seed' in c else ['--link-config',str(out/(c['name']+'_link.json'))]
            result=subprocess.run(command,cwd=repo,capture_output=True);(out/(name+'.stdout.txt')).write_bytes(result.stdout);(out/(name+'.stderr.txt')).write_bytes(result.stderr)
            invocation=root/'runs'/(args.name+'_'+name);session=next((invocation/'sessions').iterdir());item['session']=str(session)
            require(result.returncode==0,'native trial failed')
            effective=read(session/'config.effective.json');expected=read(invocation/'requested_config.json')
            require(effective==expected and effective['ip_budget']==budget and effective['link_model']==c['link'],'common configuration changed')
            require(read(invocation/'launch.json')['executable_sha256']==executable,'launch executable changed')
            hashes=read(invocation/'source_hashes.json');require(all(hashes[k]==v for k,v in native.items()),'native source changed')
            item['baseline']=baseline_audit(session);item['integration']=integration_audit(session,out/name)
            item['budget']=budget_audit(session);item['decode']=decode_audit(session);item['state']=state_audit(session)
            conservation=stage_conservation(session);save(out/name/'frame_terminal.json',conservation);item['frame_terminal_counts']=conservation['counts']
            require(not item['integration']['allowance_violations'],'pose allowance violations')
            if c['name'].endswith('_normal'):require(item['integration']['motion']['outcome']=='success','normal task failed')
            item['passed']=True
        except Exception as e:item['error']=str(e)
        item['finished_at']=stamp();save(out/'report.json',report);render(out,report)
        print(json.dumps(dict(completed=name,passed=item['passed'],error=item.get('error'),outcome=item.get('integration',{}).get('motion',{}).get('outcome'))),flush=True)
    training=[c for c in cases if c['phase']=='training']
    for ci,c in enumerate(training):
        if ci==0:run(c,'B0',.1,'training')
        for weight in (lambdas if ci==0 else list(reversed(lambdas))):run(c,'B1',weight,'training')
        if ci==1:run(c,'B0',.1,'training')
    if not all(r['passed'] for r in report['trials']):
        report['error']='training audit failed; no selection or holdout tuning permitted';save(out/'report.json',report);return 1
    scores=[]
    for weight in lambdas:
        selected=[r for r in report['trials'] if r['mode']=='B1' and r['lambda']==weight]
        successes=sum(r['integration']['motion']['outcome']=='success' for r in selected)
        seconds=sum(r['integration']['motion']['terminal']['simulation_s'] if r['integration']['motion']['outcome']=='success' else 60 for r in selected)
        size=sum(r['budget']['summary']['attempted_ip_bytes'] for r in selected)
        scores.append(dict(**{'lambda':weight},successes=successes,terminal_seconds=seconds,attempted_ip_bytes=size))
    chosen=min(scores,key=lambda s:(-s['successes'],s['terminal_seconds'],s['attempted_ip_bytes'],lambdas.index(s['lambda'])))
    selection=dict(frozen_at=stamp(),**chosen,training_scores=scores,plan_sha256=sha(out/'plan.json'),lock_sha256=sha(out/'baseline_lock.json'),holdout_used=False)
    save(out/'selection.json',selection);report['selection']=selection;save(out/'report.json',report)
    for ci,c in enumerate(c for c in cases if c['phase']=='validation'):
        for mode in (('B0','B1') if ci%2==0 else ('B1','B0')):run(c,mode,.1 if mode=='B0' else chosen['lambda'],'validation')
    report['complete']=len(report['trials'])==20;report['passed']=report['complete'] and all(r['passed'] for r in report['trials'])
    report['outcomes']=dict(Counter(r.get('integration',{}).get('motion',{}).get('outcome','invalid') for r in report['trials']))
    save(out/'report.json',report);render(out,report)
    return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
