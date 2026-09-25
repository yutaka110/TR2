"""Freeze G2-05 evidence after independent re-audit and native hash comparison."""
from pathlib import Path
import hashlib
from verify_reach_closed_loop import read,save
from verify_reach_command import require
from verify_reach_state import state_audit
from verify_reach_budget import budget_audit

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts/reach_g2_feedback_20260922';v=root/'verification';out=v/'acceptance_01'
    executable=sha(root/'bin/Release/GE3.exe');checks={};native_hash_checks=0
    for name in ('acceptance_01','decode_regression_01'):
        report=read(v/name/'report.json');require(report['passed'] and report['executable_sha256']==executable,'invalid report '+name);checks[name]=True
        require(len(report['cases'])==8,'case count')
        for case in report['cases']:
            session=Path(case['session']);invocation=session.parent.parent
            require(read(invocation/'launch.json')['executable_sha256']==executable,'launch hash differs')
            for relative,digest in read(invocation/'source_hashes.json').items():
                if relative.startswith(('research/','network/')) and Path(relative).suffix in ('.cpp','.h'):
                    require(sha(repo/relative)==digest and sha(invocation/'research_source'/relative)==digest,'native snapshot mismatch '+relative);native_hash_checks+=1
            if name=='acceptance_01':
                require(state_audit(session)==case['state'],'independent state re-audit changed');require(budget_audit(session)['passed'],'budget re-audit failed')
    for name in ('units_final_01','legacy_regression_01'):
        report=read(v/name/'report.json');require(report['passed'],'regression failed '+name);checks[name]=True
        if name.startswith('legacy'):require(report['executable_sha256']==executable,'legacy binary differs')
    require(read(out/'auditor_fault_tests.json')['passed'],'mutation checks failed');checks['auditor_fault_tests']=True
    report=read(out/'report.json');totals={key:sum(c['state'][key] for c in report['cases']) for key in ('receiver_reports_audited','causal_estimates_audited','notification_ip_bytes','stale_estimates','unknown_estimates','usable_estimates')}
    save(out/'evidence_audit.json',dict(passed=True,executable_sha256=executable,native_snapshot_hash_checks=native_hash_checks,checks=checks,totals=totals))
    save(out/'task_decision.json',dict(task='G2-05',passed=True,status='complete',executable_sha256=executable,checks=checks,totals=totals,
        progress=dict(g2_completed=5,g2_total=6,overall_completed=18,overall_total=38,gates_passed=2,gates_total=7),next_task='G2-06',
        scope='budgeted reverse UDP notification and causal sender estimation; not task success, G1 full matrix revalidation or G4 policy superiority'))
    print(dict(passed=True,native_hash_checks=native_hash_checks,totals=totals))
if __name__=='__main__':main()
