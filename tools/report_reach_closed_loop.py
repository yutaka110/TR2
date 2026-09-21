"""Render measured G1-06 results and trajectories as standalone HTML/SVG."""
import argparse
import csv
import html
import json
import os
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    source = args.report.resolve()
    report = json.loads(source.read_text(encoding="utf-8"))
    output = source.parent
    cases = report["cases"]
    passed = sum(c["passed"] for c in cases)
    table = []
    plots = []
    for task in ("T1", "T2"):
        # Both axes use the same 130 pixels per metre.
        sx = lambda x: 55+130*x
        sy = lambda y: 100-130*y
        svg = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 605 185" role="img" aria-label="'+task+' measured trajectories">',
               '<rect width="605" height="185" fill="white"/>',
               '<style>text{font:12px sans-serif;fill:#334155}</style>',
               '<text x="15" y="20">'+task+' / x,y [m] / equal axis scale</text>']
        for x in range(5):
            svg += [f'<path d="M {sx(x)} 35 V 155" stroke="#e2e8f0"/>', f'<text x="{sx(x)}" y="175">{x}</text>']
        for y in (-.375, 0, .375):
            svg += [f'<path d="M 55 {sy(y)} H 575" stroke="#e2e8f0"/>', f'<text x="2" y="{sy(y)+4}">{y:g}</text>']
        if task == "T1":
            for y in (-.375, .375):
                svg.append(f'<path d="M {sx(0)} {sy(y)} H {sx(3)}" stroke="#334155" stroke-width="3"/>')
            svg.append(f'<rect x="{sx(3.2)}" y="{sy(.375)}" width="{.15*130}" height="{.75*130}" fill="#d1fae5"/>')
        else:
            svg.append(f'<circle cx="{sx(2)}" cy="{sy(0)}" r="13" fill="#d1fae5"/>')
        for c in (c for c in cases if c["task"] == task):
            status = "成功・監査合格" if c["passed"] else "無効" if c.get("execution", {}).get("status") != "command_udp_completed" else "不合格"
            color = "#087f5b" if c["passed"] else "#b45309" if status == "無効" else "#b91c1c"
            session = Path(c["session"]) if "session" in c else None
            if session and (session / "world.csv").exists():
                with (session / "world.csv").open(encoding="utf-8", newline="") as stream:
                    states = list(csv.DictReader(stream))
                points = ' '.join(f'{sx(float(r["x_m"])):.2f},{sy(float(r["y_m"])):.2f}' for r in states[::10])
                svg.append(f'<polyline points="{points}" fill="none" stroke="{color}" stroke-width="1.5" opacity=".7"><title>{html.escape(c["id"])}</title></polyline>')
            result_time = f'{c["success_s"]:.2f} s' if c["passed"] else "—"
            if not c["passed"] and c.get("observed_success_s") is not None:
                result_time = f'{c["observed_success_s"]:.2f} s（ログ上）'
            final = c.get("observed_final_state", {})
            x = f'{float(final["x_m"]):.4f}' if "x_m" in final else "—"
            link = os.path.relpath(session / "world.csv", output).replace("\\", "/") if session else ""
            table.append(f'<tr><td>{task}</td><td>{c["y_m"]:+.2f}</td><td>{c["yaw_deg"]:+d}°</td><td style="color:{color}">{status}</td><td>{result_time}</td><td>{x}</td><td>{html.escape(c.get("error", "60秒窓・画像/指令・運動・保持を監査済み"))}</td><td><a href="{html.escape(link)}">世界ログ</a></td></tr>')
        svg.append('</svg>')
        name = task.lower()+"_trajectories.svg"
        (output / name).write_text(''.join(svg), encoding="utf-8")
        plots.append(f'<img src="{name}" alt="{task} 実測軌跡。線はロボット中心、円形の機体外形は省略。"/>')
    decision = "18条件すべて合格" if report["passed"] else "G1通過条件を満たしていません" if report["complete"] else "検証中 / 部分結果"
    badge_color = "#087f5b" if report["passed"] else "#b45309"
    audit_path = output / "evidence_audit.json"
    audit_note = ""
    if audit_path.exists():
        evidence = json.loads(audit_path.read_text(encoding="utf-8"))
        violations = evidence.get("allowance_violations", [])
        if violations:
            audit_note = f'<p style="color:#b45309">追加監査：採用した画像推定のうち{len(violations)}枚で、設定した誤差余裕を超えました。安全保証として使える誤差上限ではありません。<a href="evidence_audit.json">詳細</a></p>'
    page = f'''<!doctype html><html lang="ja"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Reach-RT G1-06 検証結果</title><style>body{{font:16px/1.7 "Yu Gothic",sans-serif;background:#eef2f6;color:#172b45;margin:0}}main{{max-width:1200px;margin:32px auto;padding:24px;background:white}}h1{{font-size:28px}}.badge{{font-size:24px;color:#087f5b}}.plots{{display:flex;flex-wrap:wrap}}img{{width:50%;min-width:350px}}table{{width:100%;border-collapse:collapse;font-size:14px}}td,th{{text-align:left;border-bottom:1px solid #dce3eb;padding:9px}}code{{overflow-wrap:anywhere}}a{{color:#175cad}}</style>
<main><h1>Reach-RT / G1-06 閉ループ検証</h1><p class="badge" style="color:{badge_color}">{decision} — {passed} / {len(cases)} 条件</p>
<p>既知マーカー・較正済みカメラ・単一PCの理想UDP回線。各条件60秒、物理100 Hz・撮影30 Hz・制御20 Hz。C++で実験し、Pythonで記録を独立監査。</p>
<p>成功条件はT1：出口領域 x=3.20〜3.35 m、T2：目標距離≤0.10 m・yaw≤5°。ともに速度≤0.02 m/sを1秒保持し、接触・場外なし。研究方式の優位性や実機安全性の証明ではありません。</p>
{audit_note}
<div class="plots">{''.join(plots)}</div><p>線はロボット中心の実測軌跡（0.1秒ごと）。両軸は同じ縮尺。緑は目標領域。機体半径0.20 mは図から省略。失敗・無効試行の途中軌跡も表示します。</p>
<table><thead><tr><th>作業</th><th>初期y [m]</th><th>初期yaw</th><th>判定</th><th>成功時刻</th><th>最終x [m]</th><th>根拠 / 理由</th><th>証拠</th></tr></thead><tbody>{''.join(table)}</tbody></table>
<p>実行exe SHA-256：<code>{report['executable_sha256']}</code></p><p><a href="plan.json">事前指定条件</a> / <a href="report.json">機械可読な全結果</a></p>
<p>各姿勢1回の確認です。成功確率・負荷変動への頑健性・他PCでの再現性は未評価。G1全体の判定は前段G1-01〜05の証拠と合わせて説明書へ記録します。</p></main></html>'''
    (output / "index.html").write_text(page, encoding="utf-8")
    print(output / "index.html")


if __name__ == "__main__":
    main()
