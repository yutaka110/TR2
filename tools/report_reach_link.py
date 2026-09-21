"""Build a readable G2-01 evidence index without treating loss tests as task success."""
import argparse
import html
import json
import os
from pathlib import Path


def main():
    parser=argparse.ArgumentParser();parser.add_argument('report',type=Path);args=parser.parse_args()
    report=json.loads(args.report.read_text(encoding='utf-8'));out=args.report.resolve().parent
    labels={'wide':'広い回線','uplink_pause':'映像方向の停止・復帰','downlink_pause':'指令方向の停止・復帰','uplink_overflow':'映像方向の低容量・溢れ','downlink_mid_packet':'指令送信途中の容量変更'}
    lines=[]
    for c in report['cases']:
        u,d=c.get('uplink',{}),c.get('downlink',{});session=Path(c['session']);base=os.path.relpath(session,out).replace('\\','/')
        result='合格' if c['passed'] else '不合格'
        lines.append(f'<tr><td>{labels[c["name"]]}</td><td>{result}</td><td>{u.get("offered_packets","—")}</td><td>{u.get("tail_dropped_packets","—")}</td><td>{d.get("tail_dropped_packets","—")}</td><td>{c.get("command",{}).get("decoded_images","—")}</td><td><a href="{html.escape(base)}/uplink_link.csv">上り</a> / <a href="{html.escape(base)}/downlink_link.csv">下り</a> / <a href="{html.escape(base)}/world.csv">運動</a></td></tr>')
    passed=sum(c['passed'] for c in report['cases']);status='合格' if report['passed'] else '未合格'
    page=f'''<!doctype html><html lang="ja"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Reach-RT G2-01 検証結果</title>
<style>body{{margin:0;background:#eef3f8;color:#193149;font:16px/1.75 "Yu Gothic UI",sans-serif}}header{{background:#13273f;color:white;padding:32px}}main{{max-width:1150px;margin:auto;padding:28px}}.card{{background:white;padding:24px;margin-bottom:22px;border-radius:10px}}.status{{font-size:28px;color:#08785c}}.flow{{padding:18px;background:#e7f2fa;font-weight:600}}table{{width:100%;border-collapse:collapse}}th,td{{padding:12px;text-align:left;border-bottom:1px solid #dbe4ed}}a{{color:#1764ae}}code{{overflow-wrap:anywhere}}.table{{overflow:auto}}</style>
<header><h1>Reach-RT / G2-01</h1><p>双方向の容量・直列化・有限キュー</p></header><main>
<section class="card"><div class="status">回線モデル検証 {status} — {passed} / {len(report['cases'])} ケース</div><p>同一C++アプリの実H.264映像とRCMD指令を、独立した上り・下りのUDP中継へ接続しました。</p><p class="flow">実UDP到着 → IP長でキュー検査 → 容量の積分で直列化 → 実UDP送信 → 復号・指令期限監視</p>
<p>指令停止ケースは6秒、他は各4秒。物理100 Hz・撮影30 Hz・制御20 Hz。回線モデルの合格と、ロボットの作業成功は別の判定です。意図的な容量不足では画像が0枚でも、期待した破棄と停止を検査します。</p></section>
<section class="card table"><h2>全ケースと原記録</h2><table><thead><tr><th>条件</th><th>モデル判定</th><th>映像パケット入力</th><th>映像tail-drop</th><th>指令tail-drop</th><th>復号画像</th><th>ログ</th></tr></thead><tbody>{''.join(lines)}</tbody></table></section>
<section class="card"><h2>何を保証する検査か</h2><p>パケットごとに容量積分を独立再計算し、送信完了予定時刻・キュー占有・tail-drop・バイト保存則を照合。元の送信と中継入口、中継出口と最終受信を照合し、モデル外のUDP紛失を隠しません。</p><p>キューは送信中の全IP長を含みます。実送信は予定時刻以降で、OSの実行遅れは別記録です。未利用RNVP応答・共通予算・乱数障害・方式比較は今回の対象範囲に含めていません。</p><p>実行exe SHA-256：<code>{report['executable_sha256']}</code></p><p><a href="report.json">全結果JSON</a> / <a href="task_decision.json">G2-01完了判定</a></p></section></main></html>'''
    (out/'index.html').write_text(page,encoding='utf-8');print(out/'index.html')


if __name__=='__main__':main()
