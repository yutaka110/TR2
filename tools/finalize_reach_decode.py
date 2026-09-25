"""Bind G2-04 acceptance, regressions and source snapshots to one executable."""
import argparse
import hashlib
from pathlib import Path
from verify_reach_closed_loop import read, save
from verify_reach_command import require


def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--build-name',default='reach_g2_decode_20260922');args=parser.parse_args()
    require(args.build_name.replace('_','').replace('-','').isalnum(),'invalid build name')
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/args.build_name;v=root/'verification';out=v/'acceptance_final_02'
    executable=sha(root/'bin/Release/GE3.exe');checks={};snapshots=0
    reports={name:read(v/name/'report.json') for name in ('acceptance_final_02','command_regression_01','budget_regression_final_01','units_01')}
    for name,r in reports.items():
        require(r['passed'],name+' failed');checks[name]=True
        if name=='units_01':continue
        require(r['executable_sha256']==executable,'report executable differs: '+name)
        for c in r['cases']:
            session=Path(c['session']);invocation=session.parent.parent
            require(read(invocation/'launch.json')['executable_sha256']==executable,'launch executable differs')
            hashes=read(invocation/'source_hashes.json')
            for relative,digest in hashes.items():
                if relative.startswith(('network/','research/')) and Path(relative).suffix in ('.h','.cpp'):
                    require(sha(repo/relative)==digest,'current native source differs: '+relative)
                    require(sha(invocation/'research_source'/relative)==digest,'snapshot differs: '+relative);snapshots+=1
    faults=read(out/'auditor_fault_tests.json');require(faults['passed'] and len(faults['checks'])==8,'fault tests incomplete');checks['auditor_fault_tests']=True
    legacy=read(v/'legacy_regression_01'/'report.json')
    require(legacy['passed'] and legacy['executable_sha256']==executable,'legacy failed or executable differs');checks['legacy_regression_01']=True
    acceptance=reports['acceptance_final_02'];require(len(acceptance['cases'])==8,'acceptance cases missing')
    totals={key:sum(c['decode'][key] for c in acceptance['cases']) for key in ('reassembled','accepted','decoded','abandoned','display_adopted','recognized','control_used')}
    require(totals['accepted']==totals['decoded']+totals['abandoned'],'unaccounted input')
    save(out/'evidence_audit.json',dict(passed=True,executable_sha256=executable,native_snapshot_hash_checks=snapshots,checks=checks,totals=totals))
    save(out/'task_decision.json',dict(task='G2-04',status='complete',passed=True,executable_sha256=executable,checks=checks,totals=totals,
        progress=dict(g2_completed=4,g2_total=6,overall_completed=17,overall_total=38,gates_passed=2,gates_total=7),next_task='G2-05',
        scope='receiver stage and deadline accounting with actual H264 output; not task success, full reference graph, G1 18-pose revalidation or policy superiority'))
    print(dict(passed=True,hash_checks=snapshots,totals=totals))


if __name__=='__main__':main()
