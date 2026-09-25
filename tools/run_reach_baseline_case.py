"""Replay one frozen G3-01 validation case with B0 or the selected B1."""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def read(path):return json.loads(path.read_text(encoding='utf-8'))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--mode',choices=('B0','B1'),required=True)
    p.add_argument('--case',choices=('valid_T1_normal','valid_T2_normal','valid_T1_mixed','valid_T2_small_queue'),default='valid_T2_normal')
    p.add_argument('--name');p.add_argument('--show',action='store_true');p.add_argument('--print-command',action='store_true')
    args=p.parse_args();repo=Path(__file__).resolve().parents[1];build='reach_g3_baseline_v3_20260922';root=repo/'artifacts'/build;study=root/'verification/study_01'
    report=read(study/'qualified_report.json');selection=read(study/'selection.json');plan=read(study/'plan.json')
    if not report['passed'] or not read(study/'task_decision.json')['passed']:raise RuntimeError('G3-01 qualification is not complete')
    if hashlib.sha256((root/'bin/Release/GE3.exe').read_bytes()).hexdigest()!=report['executable_sha256']:raise RuntimeError('fixed executable changed')
    case=next(c for c in plan['cases'] if c['name']==args.case)
    name=args.name or 'demo_'+args.mode+'_'+datetime.now().strftime('%Y%m%d_%H%M%S')
    command=[sys.executable,str(repo/'tools/run_reach_g1.py'),'--stage','command_udp','--task',case['task'],'--duration','60',
             '--name',name,'--build-name',build,'--budget-config',str(study/'budget.json'),'--state-feedback','--packet-trace',
             '--baseline',args.mode,'--baseline-lambda',str(.1 if args.mode=='B0' else selection['lambda']),'--timeout','95']
    command+=['--trace-bundle',str(study/(case['name']+'_trace'))] if 'seed' in case else ['--link-config',str(study/(case['name']+'_link.json'))]
    if args.show:command.append('--show')
    if args.print_command:print(subprocess.list2cmdline(command));return 0
    return subprocess.call(command,cwd=repo)


if __name__=='__main__':raise SystemExit(main())
