"""Same-process lifecycle checks through the production navigation request/cleanup.

A bounded opt-in diagnostic requests transitions after rendering 30 normal frames.
Research uses real H264/UDP: short headless trials or a 20-second native UI trial.
The native trial posts the return button's WM_COMMAND; no mouse is simulated.
This verifies lifecycle and data, not visual placement or mouse hit-testing.
"""
import argparse,csv,hashlib,json,os,subprocess
from pathlib import Path

p=argparse.ArgumentParser();p.add_argument('--build-name',default='reach_live_switch_20260925');p.add_argument('--configuration',default='Debug',choices=('Debug','Development','Release'));p.add_argument('--name',default='navigation_01');p.add_argument('--cases',nargs='+',choices=('roundtrip','interrupt','missing','window'),default=['roundtrip','interrupt','missing']);a=p.parse_args()
repo=Path(__file__).resolve().parents[1];root=repo/'artifacts'/a.build_name;out=root/'verification'/a.name;out.mkdir(parents=True,exist_ok=False)
exe=root/'bin'/a.configuration/'GE3.exe';report=dict(passed=False,exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),cases=[])
try:
 for mode in a.cases:
  target=out/mode;target.mkdir();env={k:v for k,v in os.environ.items() if not k.upper().startswith(('RNVP_','TR2_NETWORK_','TR2_REACH_','TR2_RESEARCH_'))}
  env.update(TR2_RESEARCH_MODE='legacy',TR2_REACH_NAVIGATION_TEST=mode,TR2_REACH_NAVIGATION_TEST_ROOT=str(target),RNVP_DISABLE_CAMERA='1',RNVP_CODEC='h264',RNVP_PRESENT_SYNC_INTERVAL='0',TR2_NETWORK_MODE='loopback')
  startup=subprocess.STARTUPINFO();startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=subprocess.SW_HIDE
  with (target/'stdout.txt').open('wb') as stdout,(target/'stderr.txt').open('wb') as stderr:
   child=subprocess.Popen([str(exe)],cwd=repo,env=env,startupinfo=startup,stdout=stdout,stderr=stderr)
   try:code=child.wait(timeout=120)
   except subprocess.TimeoutExpired:child.kill();child.wait();raise
  events=[json.loads(s) for s in (target/'navigation.jsonl').read_text().splitlines()]
  case=dict(mode=mode,exit_code=code,pid=child.pid,events=events,sessions=[]);report['cases'].append(case)
  assert code==0,(mode,code)
  assert all(e['pid']==child.pid for e in events),'process changed'
  assert [e['event'] for e in events]==(['legacy_enter','legacy_exit','research_enter','research_launch_failed' if mode=='missing' else 'research_exit']*2+['legacy_enter','legacy_exit']),events
  assert all(e['result']==0 for e in events if e['event']=='legacy_exit'),'normal screen did not restart'
  sessions=list((target/'sessions').glob('*/summary.json'))
  assert len(sessions)==(0 if mode=='missing' else 2),'session count'
  for summary in sessions:
   s=summary.parent;result=json.loads(summary.read_text());expected='interrupted' if mode=='interrupt' else 'command_udp_completed';assert result['status']==expected,result
   with (s/'decoded_audit.csv').open(newline='',encoding='utf-8') as f:decoded=list(csv.DictReader(f))
   assert decoded and all(r['identity_match']=='1' for r in decoded),'no matched received image'
   with (s/'world.csv').open(newline='',encoding='utf-8') as f:world=list(csv.DictReader(f))
   assert any(float(w['v_m_s'])>0 for w in world),'robot did not move'
   assert (s/'scheduler.csv').is_file(),'G4 scheduler not connected'
   if mode=='window':
    events=[json.loads(line) for line in (s/'events.jsonl').read_text().splitlines()]
    assert any(e['event']=='live_return_button_created' for e in events),'no return button'
    assert any(e['event']=='live_ui_paints' and e['count']>1 for e in events),'window was not painted'
   case['sessions'].append(dict(directory=str(s),status=result['status'],decoded=len(decoded),moved=True))
  case['passed']=True;print(json.dumps(case),flush=True)
 report['passed']=True
except Exception as e:report['error']=repr(e);print(repr(e),flush=True)
report['scope']='same-process lifecycle diagnostic; window case paints real UI, other cases use headless research; no mouse/visual verification or performance claim'
(out/'report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
raise SystemExit(0 if report['passed'] else 1)
