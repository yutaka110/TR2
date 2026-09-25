"""G2-06: preregister full windows; join actual network, images and robot outcomes.

Offline evaluator only. Never feeds truth or future traces back to the experiment.
Task failure is an outcome, not permission to discard a trial or relax its audit.
"""
import argparse
import bisect
from collections import Counter, defaultdict
import copy
import csv
import hashlib
import html
import json
import math
from pathlib import Path
import subprocess
import sys

from verify_reach_closed_loop import read, rows, save
from verify_reach_command import require, wire
from verify_reach_budget import budget_audit
from verify_reach_decode import decode_audit
from verify_reach_state import state_audit
from generate_reach_trace import write_bundle


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_csv(path, data):
    require(bool(data), 'empty joined table')
    with path.open('w', encoding='utf-8', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(data[0]))
        writer.writeheader(); writer.writerows(data)


def controller_audit(commands, observations, recognized, task, width):
    """Recompute the fixed controller using its recorded image inputs only."""
    align = False; last_frame = last_capture = 0; stream = 0
    for index, c in enumerate(commands, 1):
        now = int(c['generated_us']); fid = int(c['source_frame_id'])
        require(int(c['sequence']) == index and int(c['valid_until_us']) == now+100000, 'controller sequence/lifetime')
        v = w = distance = margin = 0.; phase = 'OBSERVE'; complete = False
        if not fid:
            reason = 'no_image'; align = False
            require(int(c['source_capture_us']) == int(c['source_stream_id']) == 0, 'unobserved image metadata')
        else:
            require(fid in observations and fid in recognized, 'command has no real recognition')
            o = observations[fid]
            require(int(recognized[fid]['event_us']) <= now, 'command uses recognition from the future')
            capture = int(o['capture_us']); age = (now-capture)/1000000
            require(int(o['received_us']) <= now and capture <= int(o['received_us']), 'recognition clock provenance')
            require(capture == int(c['source_capture_us']) and c['source_stream_id'] == o['stream_id'], 'controller source metadata')
            require(abs(float(c['age_ms'])-age*1000) < 1e-7, 'control AoI mismatch')
            if o['valid'] != '1':
                reason = o['reason']; align = False
            elif now-capture > 200000:
                reason = 'observation_expired'; align = False
            else:
                if stream != int(o['stream_id']):
                    stream = int(o['stream_id']); last_frame = last_capture = 0; align = False
                require(fid >= last_frame and capture >= last_capture, 'controller observation order')
                last_frame, last_capture = fid, capture
                x, y, yaw = (float(o[k]) for k in ('x_m', 'y_m', 'yaw_rad'))
                distance = math.hypot((3.25 if task == 'T1' else 2)-x, -y)
                alpha = math.remainder(math.atan2(-y, (3.25 if task == 'T1' else 2)-x)-yaw, 2*math.pi)
                enter, leave = (.025, .04) if task == 'T1' else (.08, .10)
                if distance <= enter: align = True
                elif distance > leave: align = False
                reason = 'image_feedback'
                if align:
                    phase = 'ALIGN'; w = max(-.8, min(.8, -1.5*yaw))
                    if distance <= enter and abs(yaw) <= math.radians(3) and o['speed_valid'] == '1' and float(o['speed_m_s']) <= .02:
                        phase = 'HOLD'; w = 0.; complete = True
                else:
                    phase = 'APPROACH'; w = max(-.8, min(.8, 1.5*alpha))
                    v = min(.3, .5*distance)*max(0., math.cos(alpha)) if abs(alpha) <= math.pi/6 else 0.
                error = float(o['position_error_m'])+.3*age+.15*age*age
                if error > .25 or float(o['yaw_error_rad']) > .3:
                    v = w = 0.; phase = 'OBSERVE'; reason = 'uncertainty_stop'; complete = False
                else:
                    heading = min(math.pi/2, abs(yaw)+float(o['yaw_error_rad'])+.8*age)
                    lateral = float(o['position_error_m'])+(.3*age+.15*age*age)*math.sin(heading)
                    margin = width/2-.2-abs(y)-lateral-(.3*.1+.3*.3/(2*.6))*abs(math.sin(yaw))
                    if task == 'T1' and x < 3.20 and margin < .02:
                        v = 0.; w = max(-.8, min(.8, -1.5*yaw)); reason = 'wall_margin_stop'; complete = False
        require(c['reason'] == reason and c['state'] == phase and int(c['estimated_complete']) == int(complete), 'controller phase/reason differs from image-only replay')
        for field, expected in [('v_m_s', v), ('w_rad_s', w), ('distance_m', distance), ('wall_margin_m', margin)]:
            require(abs(float(c[field])-expected) < 2e-8, 'controller differs from image-only replay: '+field)
    return len(commands)


def world_audit(session, config, applications, states):
    """Reintegrate every step, including contact, goal hold and terminal freeze."""
    duration = config['duration_s']; task = config['task']; settings = config['world']
    previous = dict(x=0., y=settings['initial_y_m'], yaw=settings['initial_yaw_rad'], v=0., w=0.)
    collision = outside = success = timeout = False; hold = 0.; first_terminal = None
    width = settings['corridor_width_m']; clearance = float('inf'); max_residual = 0.
    require(len(states) == len(applications) == round(duration*100), 'full physics window missing')
    for tick, (s, a) in enumerate(zip(states, applications), 1):
        require(int(s['physics_tick']) == int(a['physics_tick']) == tick, 'physics join identity')
        require(abs(float(s['simulation_s'])-tick*.01) < 1e-8, 'simulation clock mismatch')
        x, y, yaw = previous['x'], previous['y'], previous['yaw']; v = w = 0.
        if not (collision or outside or success or timeout):
            tv, tw = float(a['v_m_s']), float(a['w_rad_s'])
            dv = .006 if tv < previous['v'] else .003
            v = previous['v']+max(-dv, min(dv, tv-previous['v']))
            w = previous['w']+max(-.016, min(.016, tw-previous['w']))
            turn = (previous['w']+w)*.005; travel = (previous['v']+v)*.005
            nx, ny = x+travel*math.cos(yaw+turn*.5), y+travel*math.sin(yaw+turn*.5)
            if task == 'T1':
                gap = min(math.hypot(nx-max(0., min(3., nx)), ny-wall)-.2 for wall in (-width/2, width/2))
                clearance = min(clearance, gap); collision = gap <= 0
            if not collision: x, y, yaw = nx, ny, math.remainder(yaw+turn, 2*math.pi)
            outside = x < -.5 or x > 4.5 or abs(y) > 2
            goal = 3.20 <= x <= 3.35 if task == 'T1' else math.hypot(x-2, y) <= .1 and abs(yaw) <= math.radians(5)
            hold = hold+.01 if goal and v <= .02 and not collision and not outside else 0.
            success = hold >= 1.; timeout = not success and tick*.01 >= 60.-1e-8
            if collision or outside or success or timeout:
                first_terminal = dict(tick=tick, simulation_s=tick*.01, applied_us=int(a['applied_us']),
                                      outcome='collision' if collision else 'out_of_bounds' if outside else 'success' if success else 'timeout')
                v = w = 0.
        expected = dict(x_m=x, y_m=y, yaw_rad=yaw, v_m_s=v, w_rad_s=w, goal_hold_s=hold)
        for field, value in expected.items():
            residual = abs(float(s[field])-value); max_residual = max(max_residual, residual)
            require(math.isfinite(float(s[field])) and residual < 3e-8, 'independent world integration: '+field)
        for field, value in [('collision', collision), ('out_of_bounds', outside), ('success', success), ('timeout', timeout)]:
            require(int(s[field]) == int(value), 'independent task outcome: '+field)
        previous = dict(x=x, y=y, yaw=yaw, v=v, w=w)
    evaluation = read(session/'visual_control_summary.json')
    require(evaluation['local_evaluator_success'] == success, 'task summary contradicts world')
    truth = rows(session/'capture_evaluation.csv'); captures = {int(c['frame_id']):c for c in rows(session/'captures.csv')}
    require(len(truth) == len(captures) == round(duration*30), 'full camera/evaluation window missing')
    for t in truth:
        tick = int(t['physics_tick']); fid = int(t['frame_id'])
        require(1 <= tick <= len(states) and fid in captures, 'capture/world identity')
        require(int(captures[fid]['capture_us']) >= int(applications[tick-1]['applied_us']), 'capture precedes its world state')
        for key in ('x_m','y_m','yaw_rad'):
            require(abs(float(t[key])-float(states[tick-1][key])) < 1e-10, 'capture truth differs from physics tick')
    return dict(physics_steps=len(states), outcome=first_terminal['outcome'] if first_terminal else 'window_ended',
                terminal=first_terminal, final_pose=previous, max_integration_residual=max_residual,
                minimum_wall_clearance_m=clearance if task == 'T1' else None)


def integration_audit(session, output=None):
    config = read(session/'config.effective.json'); execution = read(session/'summary.json')
    duration = config['duration_s']; origin = read(session/'state_model.json')['origin_us']
    require(execution['status'] == 'command_udp_completed', 'native trial incomplete')
    require(execution['clock_tick_counts'] == dict(physics=round(duration*100), camera=round(duration*30), control=round(duration*20)), 'incomplete common window')
    require(config.get('state_feedback') is True and config['command_link']['scenario'] == 'normal', 'integration requires ordinary budgeted commands and state feedback')
    events = rows(session/'decode_events.csv'); observations = {int(o['frame_id']):o for o in rows(session/'observations.csv')}
    commands = rows(session/'commands.csv'); states = rows(session/'world.csv'); applications = rows(session/'udp_applied_commands.csv')
    require(len(commands) == round(duration*20), 'full control window missing')
    recognized = {int(e['frame_id']):e for e in events if e['event'].startswith('recognition_')}
    stages = defaultdict(dict)
    for e in events:
        if not e['event'].startswith('control_'): stages[int(e['frame_id'])][e['event']] = int(e['event_us'])
    captures = {int(c['frame_id']):c for c in rows(session/'captures.csv')}
    encoded = {int(e['frame_id']):e for e in rows(session/'encoded.csv')}
    require(set(captures) == set(encoded), 'encoder dropped capture silently')
    controls_checked = controller_audit(commands, observations, recognized, config['task'], config['world']['corridor_width_m'])
    motion = world_audit(session, config, applications, states)
    packets = defaultdict(Counter); delivered = defaultdict(list)
    uplink = rows(session/'uplink_link.csv'); arrivals = {r['packet_id']:r for r in uplink if r['event'] in ('admitted','tail_drop')}
    for r in uplink:
        if r['event'] not in ('delivered','trace_drop','tail_drop','cancelled_at_close','propagation_cancelled_at_close'): continue
        a = arrivals[r['packet_id']]; b = bytes.fromhex(a['wire_hex']); fid = int.from_bytes(b[16:20], 'big')
        if b[5] not in (0,6): continue
        require(fid in captures, 'video packet without capture')
        require(origin+int(a['arrival_us']) >= int(encoded[fid]['encoder_output_us']) >= int(captures[fid]['capture_us']), 'uplink packet precedes encoder output')
        packets[fid][r['event']] += 1
        if r['event'] == 'delivered': delivered[fid].append(origin+int(r['actual_send_us']))
    frame_rows = []
    for fid, c in captures.items():
        st = stages[fid]; capture = int(c['capture_us']); complete = st.get('reassembled', 0)
        if complete:
            require(any(t <= complete for t in delivered[fid]), 'reassembly without prior delivered image data')
        prev = capture
        for event in ('reassembled','decode_dequeued','au_validated','input_accepted','decoded_output'):
            if event in st:
                require(st[event] >= prev, 'pipeline source clocks reversed: '+event); prev = st[event]
        if fid in recognized:
            require(st.get('decoded_output', 0) and st['decoded_output'] <= int(observations[fid]['received_us']) <= int(recognized[fid]['event_us']), 'recognition without prior actual pixels')
        frame_rows.append(dict(frame_id=fid, capture_s=(capture-origin)/1e6,
            reassembled_s=(complete-origin)/1e6 if complete else '', decoded_s=(st['decoded_output']-origin)/1e6 if st.get('decoded_output') else '',
            recognition_s=(int(recognized[fid]['event_us'])-origin)/1e6 if fid in recognized else '',
            valid=observations[fid]['valid'] if fid in observations else '',
            delivered=packets[fid]['delivered'], lost=packets[fid]['trace_drop'], queue_drop=packets[fid]['tail_drop']))
    # RCMD delivery time -> actual receiver -> exact application. Existing command
    # audit checks acceptance and watchdog semantics; here join every source stage.
    down = rows(session/'downlink_link.csv'); sources = {r['packet_id']:r for r in down if r['event'] in ('admitted','tail_drop')}
    delivery = defaultdict(list)
    for r in down:
        if r['event'] == 'delivered' and sources[r['packet_id']]['wire_hex'].startswith('52434d44'):
            delivery[sources[r['packet_id']]['wire_hex']].append(origin+int(r['actual_send_us']))
    accepted = {}
    for r in rows(session/'command_rx.csv'):
        times = delivery[r['wire_hex']]; require(bool(times), 'command receive bypasses link')
        t = times.pop(0); require(t <= int(r['received_us']), 'command received before relay send')
        if r['status'] == 'accepted':
            p = wire(r['wire_hex']); accepted[p['sequence']] = dict(packet=p, received_us=int(r['received_us']), delivered_us=t)
    require(not any(delivery.values()), 'delivered commands absent from receive log')
    command_map = {int(c['sequence']):c for c in commands}; trajectory = []; aoi = []; stops = []
    for s, a in zip(states, applications):
        now = int(a['applied_us']); seq = int(a['sequence']); fid = int(a['source_frame_id'])
        require(int(a['live']) == int(a['reason'] == 'active'), 'application live flag')
        if seq:
            require(seq in accepted, 'actuation without UDP acceptance')
            p = accepted[seq]; c = command_map[seq]
            require(p['received_us'] <= now and int(a['accepted_us']) == p['received_us'], 'actuation before accepted command')
            for key, source in [('generated_us','generated_us'), ('valid_until_us','valid_until_us'), ('source_frame_id','source_frame_id'), ('source_capture_us','source_capture_us')]:
                require(a[key] == c[source], 'actuation provenance mismatch: '+key)
            if fid:
                require(int(recognized[fid]['event_us']) <= int(c['generated_us']) <= p['delivered_us'] <= p['received_us'] <= now, 'end-to-end causal order')
                if a['live'] == '1': aoi.append(now-int(c['source_capture_us']))
        previous_moving = bool(trajectory and (trajectory[-1]['v_m_s'] > 1e-9 or abs(trajectory[-1]['w_rad_s']) > 1e-9))
        moving = float(s['v_m_s']) > 1e-9 or abs(float(s['w_rad_s'])) > 1e-9
        if previous_moving and not moving:
            stops.append(dict(simulation_s=float(s['simulation_s']), wall_s=(now-origin)/1e6, reason=a['reason'], command_reason=command_map[seq]['reason'] if seq else 'no_command', terminal=s['success']=='1' or s['timeout']=='1' or s['collision']=='1' or s['out_of_bounds']=='1'))
        trajectory.append(dict(physics_tick=int(s['physics_tick']), simulation_s=float(s['simulation_s']), wall_s=(now-origin)/1e6,
            command_sequence=seq, source_frame_id=fid, application_reason=a['reason'], command_reason=command_map[seq]['reason'] if seq else 'no_command',
            capture_s=(int(a['source_capture_us'])-origin)/1e6 if fid else '', recognition_s=(int(recognized[fid]['event_us'])-origin)/1e6 if fid else '',
            command_generated_s=(int(a['generated_us'])-origin)/1e6 if seq else '', received_s=(int(a['accepted_us'])-origin)/1e6 if seq else '',
            x_m=float(s['x_m']), y_m=float(s['y_m']), yaw_rad=float(s['yaw_rad']), v_m_s=float(s['v_m_s']), w_rad_s=float(s['w_rad_s']),
            success=int(s['success']), timeout=int(s['timeout'])))
    truth = {int(r['frame_id']):r for r in rows(session/'capture_evaluation.csv')}; errors = []; violations = []
    for fid, o in observations.items():
        if o['valid'] != '1': continue
        t = truth[fid]; pe = math.hypot(float(o['x_m'])-float(t['x_m']), float(o['y_m'])-float(t['y_m'])); ye = abs(math.remainder(float(o['yaw_rad'])-float(t['yaw_rad']), 2*math.pi))
        errors.append((pe, ye))
        if pe > float(o['position_error_m']) or ye > float(o['yaw_error_rad']): violations.append(fid)
    # Report calibration failures honestly; they invalidate the G2 gate, not logs.
    if output:
        output.mkdir(parents=True, exist_ok=True); write_csv(output/'frame_chain.csv', frame_rows); write_csv(output/'actuation_chain.csv', trajectory)
    return dict(passed=True, controller_commands_replayed=controls_checked, motion=motion, frames_joined=len(frame_rows),
                applications_joined=len(trajectory), allowance_violations=violations,
                max_position_error_m=max((e[0] for e in errors), default=0), max_yaw_error_rad=max((e[1] for e in errors), default=0),
                control_loop_aoi_max_ms=max(aoi, default=0)/1000,
                application_reasons=dict(Counter(a['reason'] for a in applications)), command_reasons=dict(Counter(c['reason'] for c in commands)),
                stop_events=stops, maximum_clock_lag_ms=max((r['wall_s']-r['simulation_s'])*1000 for r in trajectory))


def direction(delay=10000, queue=65536):
    return dict(queue_ip_bytes=queue, capacity=[dict(at_us=0,bps=6000000)], impairment=[dict(at_us=0,delay_us=delay,drop=False)])


def scenarios():
    base = dict(uplink=direction(), downlink=direction())
    cases = []
    for task in ('T1','T2'): cases.append(dict(name=task+'_normal', task=task, link=copy.deepcopy(base), expect='success'))
    for direction_name, task in [('uplink','T1'),('downlink','T2')]:
        link = copy.deepcopy(base); link[direction_name]['impairment'] += [dict(at_us=2000000,delay_us=10000,drop=True),dict(at_us=3000000,delay_us=10000,drop=False)]
        cases.append(dict(name=task+'_'+direction_name+'_outage',task=task,link=link,expect='measured', outage=[2,3]))
    link = copy.deepcopy(base); link['uplink']['queue_ip_bytes'] = 8192
    link['uplink']['capacity'] += [dict(at_us=2000000,bps=750000),dict(at_us=4000000,bps=6000000)]
    cases.append(dict(name='T2_congestion',task='T2',link=link,expect='measured'))
    cases.append(dict(name='T1_seeded_mixed',task='T1',seed=206,expect='measured'))
    link = copy.deepcopy(base); link['uplink']['impairment'][0]['drop'] = True
    cases.append(dict(name='T2_no_video',task='T2',link=link,expect='timeout'))
    link = copy.deepcopy(base); link['downlink']['impairment'][0]['delay_us'] = 300000
    cases.append(dict(name='T2_late_downlink',task='T2',link=link,expect='timeout'))
    return cases


def render(out, report):
    cards = []
    for c in report['cases']:
        a = c.get('integration', {}); m = a.get('motion', {}); b = c.get('budget', {}).get('summary', {}); d = c.get('decode', {})
        detail = out/c['name']; points = []
        if (detail/'actuation_chain.csv').exists():
            data = rows(detail/'actuation_chain.csv')
            points = [f"{35+float(r['simulation_s'])*10:.1f},{135-float(r['v_m_s'])*300:.1f}" for r in data[::10]]
        graph = '<svg viewBox="0 0 660 160" role="img" aria-label="60秒間の実速度。縦軸0〜0.3m/s"><path d="M35 35V135H640" stroke="#9aabba" fill="none"/><text x="0" y="45">0.3</text><text x="15" y="138">0</text><text x="600" y="155">60秒</text><polyline fill="none" stroke="#087f8c" stroke-width="2" points="'+' '.join(points)+'"/></svg>' if points else ''
        cost = format(b['attempted_ip_bytes'], ',') if 'attempted_ip_bytes' in b else '未集計'
        cards.append(f"<section><h2>{html.escape(c['name'])} — 計測 {'PASS' if c.get('passed') else 'FAIL'}</h2><p>作業結果 <b>{html.escape(m.get('outcome','未判定'))}</b> ／ 終端時刻 {m.get('terminal',{}).get('simulation_s','—') if m.get('terminal') else '—'} 秒（シミュレーション時刻）</p><p>再構成 {d.get('reassembled','—')} → 実画素 {d.get('decoded','—')} → 認識採用 {d.get('recognized','—')} ／ 指令再計算 {a.get('controller_commands_replayed','—')} ／ 実動作照合 {a.get('applications_joined','—')}</p><p>通信量 {cost} IP byte</p>{graph}<p><a href='{c['name']}/frame_chain.csv'>画像の経路 CSV</a> ／ <a href='{c['name']}/actuation_chain.csv'>指令・実動作の経路 CSV</a></p><details><summary>停止・診断の実測値</summary><pre>{html.escape(json.dumps(a,ensure_ascii=False,indent=2))}</pre></details><p>{html.escape(c.get('error',''))}</p></section>")
    (out/'index.html').write_text('<!doctype html><html lang="ja"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>Reach-RT G2-06 統合計測</title><style>body{font:16px system-ui;background:#edf3fa;color:#17304a;margin:32px auto;padding:0 24px;max-width:1100px}section{background:white;padding:24px;margin:20px 0;border-radius:12px}h1{color:#143e67}svg{width:100%;max-width:660px}pre{white-space:pre-wrap}a{color:#075aa1}</style><h1>Reach-RT / 障害から作業結果まで</h1><p>実H.264・双方向UDP・受信画像制御の60秒試行。実験後のログ照合であり、未来情報を制御へ渡しません。</p><p>統合計測 '+('PASS' if report['passed'] else '未完了／FAIL')+'。作業の成功・時間切れは別の結果です。方式の性能比較や統計的成功率を示すものではありません。</p>'+''.join(cards)+'</html>',encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(); parser.add_argument('--name',required=True); parser.add_argument('--build-name',default='reach_g2_feedback_20260922'); args = parser.parse_args()
    for value in (args.name,args.build_name): require(value.replace('_','').replace('-','').isalnum(),'invalid name')
    repo = Path(__file__).resolve().parents[1]; root = repo/'artifacts'/args.build_name; out = root/'verification'/args.name; out.mkdir(parents=True,exist_ok=False)
    cases = scenarios(); executable = sha(root/'bin/Release/GE3.exe'); budget = read(repo/'config/reach_rt_g2_budget.json'); save(out/'budget.json',budget)
    for case in cases:
        if 'seed' in case:
            profile = {d:dict(queue_ip_bytes=16384,capacities_bps=[1500000,6000000] if d=='uplink' else [6000000],drop_ppm=30000,base_delay_us=10000,jitter_us=10000) for d in ('uplink','downlink')}
            _, metadata = write_bundle(out/(case['name']+'_trace'),seed=case['seed'],horizon_us=60000000,slot_us=100000,profile=profile)
            case['trace_sha256'] = metadata['link_sha256']
        else: save(out/(case['name']+'_link.json'),case['link'])
    plan = dict(version='G2-06-v1',duration_s=60,initial_y_m=0,initial_yaw_rad=0,executable_sha256=executable,
                verifier_sha256=sha(Path(__file__)),cases=cases,budget=budget,retries=0,continue_after_failure=True,
                gate_requirements=['all eight valid full windows','independent budget/link/decode/state audits','image-only controller replay','physics and outcome replay','zero pose allowance violations','normal tasks succeed','severe no-video/late-command tasks time out without motion','outage stop and recovery observed'],
                scope='integration qualification, not statistical efficacy or G1 full pose matrix')
    save(out/'plan.json',plan); report = dict(passed=False,complete=False,executable_sha256=executable,cases=[])
    for case in cases:
        name = case['name']; item = dict(name=name,task=case['task'],passed=False); report['cases'].append(item)
        print(json.dumps(dict(starting=name)),flush=True)
        try:
            command = [sys.executable,str(repo/'tools/run_reach_g1.py'),'--stage','command_udp','--task',case['task'],'--duration','60','--name',args.name+'_'+name,'--build-name',args.build_name,'--budget-config',str(out/'budget.json'),'--state-feedback','--packet-trace','--timeout','95']
            command += ['--trace-bundle',str(out/(name+'_trace'))] if 'seed' in case else ['--link-config',str(out/(name+'_link.json'))]
            result = subprocess.run(command,cwd=repo,capture_output=True); (out/(name+'.stdout.txt')).write_bytes(result.stdout); (out/(name+'.stderr.txt')).write_bytes(result.stderr)
            invocation = root/'runs'/(args.name+'_'+name); session = next((invocation/'sessions').iterdir()); item['session'] = str(session)
            require(result.returncode == 0,'native trial failed: '+str(read(session/'summary.json')))
            require(read(invocation/'launch.json')['executable_sha256'] == executable,'executable changed')
            item['integration'] = integration_audit(session,out/name)
            item['budget'] = budget_audit(session); item['decode'] = decode_audit(session); item['state'] = state_audit(session)
            a = item['integration']; require(not a['allowance_violations'],'accepted observations exceed declared pose allowance')
            if case['expect'] != 'measured': require(a['motion']['outcome'] == case['expect'],'preregistered outcome mismatch')
            if case['expect'] == 'timeout': require(a['motion']['final_pose']['x'] == 0 and not a['stop_events'],'unavailable input moved robot')
            if 'outage' in case:
                chain = rows(out/name/'actuation_chain.csv')
                require(any(2 < e['wall_s'] < 3.5 and not e['terminal'] for e in a['stop_events']),'outage did not stop moving robot')
                require(any(3 < float(r['wall_s']) < 8 and float(r['v_m_s']) > .05 for r in chain),'motion did not recover after outage')
            if name == 'T2_congestion': require(item['budget']['uplink']['tail_dropped_packets'] > 0,'congestion did not exercise finite queue')
            if 'seed' in case: require(item['budget']['uplink']['trace_dropped_packets'] > 0 and item['budget']['downlink']['trace_dropped_packets'] > 0,'mixed trace did not exercise both directions')
            item['passed'] = True
        except Exception as error: item['error'] = str(error)
        save(out/'report.json',report)
        print(json.dumps(dict(case=name,passed=item['passed'],error=item.get('error'),outcome=item.get('integration',{}).get('motion',{}).get('outcome'))),flush=True)
    report['complete'] = len(report['cases']) == len(cases); report['passed'] = report['complete'] and all(c['passed'] for c in report['cases'])
    save(out/'report.json',report); render(out,report)
    return 0 if report['passed'] else 1


if __name__ == '__main__': raise SystemExit(main())
