"""Fixed grid: causal optimal terminal success versus native short-image-use rank."""
import argparse,json,subprocess,time
from dataclasses import asdict
from fractions import Fraction as F
from itertools import product
from pathlib import Path
from reach_exact_model import Model,Exact,MODES
from reach_scheduler_exact import RestrictedScheduler,independent_proxy
from test_reach_exact import exhaustive
from run_reach_exact_study import save,sha
def main():
    p=argparse.ArgumentParser();p.add_argument('--name',default='exact_01');p.add_argument('--build-name',default='reach_g4_comparison_20260925');p.add_argument('--build',default='build_01');a=p.parse_args()
    repo=Path(__file__).resolve().parents[1];out=repo/'artifacts'/a.build_name/'verification'/a.name;out.mkdir(parents=True,exist_ok=False)
    exe=out.parent/a.build/'comparison_bin/reach_comparison_probe.exe';cases=[]
    for h,b,d,q in product((2,3),(1,2,3,4),(0,1),(F(0),F(1))):cases.append((f'h{h}_b{b}_d{d}_loss{q}',Model(horizon=h,budget_packets=b,delay=d,report_loss=q)))
    for r,s,pv,b in ((1,0,F(3,4),3),(0,0,F(3,4),3),(1,1,F(3,4),2),(0,1,F(3,4),2),(1,0,F(1),3),(1,0,F(0),3),(0,0,F(1),3),(1,1,F(1,2),0)):
        cases.append((f'known_r{r}_s{s}_p{pv}_b{b}',Model(horizon=2,budget_packets=b,good=pv,bad=pv,initial=((r,s,0,F(1)),))))
    save(out/'plan.json',dict(cases=[dict(name=n,model=asdict(m)) for n,m in cases],modes=MODES,lambda_value=F(1,10),scale_ip_bytes=65536,restriction='WAIT / P / repeated P / immediate two-packet I; same G3 transitions, no live XOR or IDR-generation mapping',objective='causal terminal success; proxy uses exact one-step active-task progress probability, removing empirical prediction error',retries=0))
    report=dict(passed=False,rows=[],independent_exact=0,independent_proxy=0);ranks={}
    for ci,(name,model) in enumerate(cases):
        for mode in MODES:
            start=time.perf_counter();optimal=Exact(model,mode).run();exact_s=time.perf_counter()-start
            solver=RestrictedScheduler(model,mode);start=time.perf_counter();proxy=solver.run();proxy_s=time.perf_counter()-start
            assert proxy['success']<=optimal['success'];ranks.update(solver.ranks)
            row=dict(case=name,mode=mode,model=asdict(model),optimal=optimal['success'],proxy=proxy['success'],regret=optimal['success']-proxy['success'],optimal_ip=optimal['expected_ip_bytes'],proxy_ip=proxy['expected_ip_bytes'],exact_seconds=exact_s,proxy_audit_seconds=proxy_s,
                reachable_beliefs=len(solver.visits),non_tie_mismatches=sum(v['non_tie_mismatch'] for v in solver.visits),decisions=solver.visits)
            if ci in (0,2,8,10):
                value,_,_=exhaustive(model,mode);assert value==optimal['success'];report['independent_exact']+=1
                assert independent_proxy(model,mode)==proxy['success'];report['independent_proxy']+=1
            report['rows'].append(row)
        print(name+' complete',flush=True)
    lines=[];items=list(ranks.items())
    for (weight,signature),selected in items:
        lines.append(f'{len(signature)} {float(F(weight))} 65536 '+' '.join(f'{float(F(p))} {ip} 1' for p,ip in signature))
    proc=subprocess.run([str(exe),'rank'],input='\n'.join(lines)+'\n',text=True,capture_output=True,check=True);(out/'native_input.txt').write_text('\n'.join(lines)+'\n');(out/'native_output.txt').write_text(proc.stdout)
    outputs=proc.stdout.splitlines();assert len(outputs)==len(items)
    for line,(_,selected) in zip(outputs,items):assert int(line.split(',')[0])==selected,'native ranking mismatch'
    report.update(passed=True,native_rank_queries=len(items),native_exe_sha256=sha(exe),mean_regret=sum(r['regret'] for r in report['rows'])/len(report['rows']),max_regret=max(r['regret'] for r in report['rows']),equal_rows=sum(r['regret']==0 for r in report['rows']),worse_rows=sum(r['regret']>0 for r in report['rows']),scope='exact finite model only; actual shared native rank checked at every unique reachable decision; proxy audit runtime includes optimal-Q diagnostics, not live latency')
    save(out/'report.json',report);print(json.dumps({k:str(v) for k,v in report.items() if k!='rows'}));return 0
if __name__=='__main__':raise SystemExit(main())
