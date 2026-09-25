"""Create a standalone, evidence-linked G2-03 result viewer."""
import argparse,html,json,os
from pathlib import Path


def main():
    p=argparse.ArgumentParser();p.add_argument('report',type=Path);args=p.parse_args();r=json.loads(args.report.read_text(encoding='utf-8'));out=args.report.resolve().parent
    names={'all_classes':'映像・修復・応答の全種別','aggregate_cap':'共通10,000 byte上限','aggregate_rate':'共通128 kbps・4 KiBバースト','downlink_cap_stop':'下り26,000 byte上限と停止','loss_charged':'損失しても送信費用を維持'}
    labels={'video_idr':'IDR映像','video_delta':'通常映像','fec':'FEC','retransmission':'再送','ack':'ACK','nack':'NACK','control':'制御応答','transport_feedback':'受信状況通知','command':'ロボット指令'}
    cases=[]
    def link(path):return html.escape(os.path.relpath(path,out).replace('\\','/'))
    for c in r['cases']:
        s=c['summary'];path=Path(c['session']);cases.append(f'<tr><td>{names[c["name"]]}</td><td>{"合格" if c["passed"] else "未合格"}</td><td>{s["uplink_ip_bytes"]:,}</td><td>{s["downlink_ip_bytes"]:,}</td><td>{s["attempted_ip_bytes"]:,}</td><td>{c["discarded_before_send_bytes"]:,}</td><td><a href="{link(path/"ip_budget.csv")}">送信台帳</a> / <a href="{link(path/"ip_budget_summary.json")}">内訳JSON</a></td></tr>')
    first=r['cases'][0]['summary'];classes=[]
    for kind,v in first['classes'].items():
        direction,name=kind.split('/');size=v['attempted_ip_bytes'];ratio=size/first['attempted_ip_bytes']*100
        classes.append(f'<tr><td>{"上り" if direction=="uplink" else "下り"}</td><td>{labels.get(name,name)}</td><td>{size:,}</td><td><div class="bar" style="width:{ratio:.3f}%"></div>{ratio:.2f}%</td></tr>')
    page=f'''<!doctype html><html lang="ja"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Reach-RT G2-03 共通IP予算</title>
<style>body{{margin:0;background:#eef3f8;color:#193149;font:16px/1.75 "Yu Gothic UI",sans-serif}}header{{background:#13273f;color:white;padding:28px}}main{{max-width:1150px;margin:auto;padding:24px}}section{{background:white;padding:24px;margin-bottom:20px;border-radius:10px}}h1{{margin:0}}.status{{font-size:26px;color:#08785c}}table{{width:100%;border-collapse:collapse}}th,td{{padding:10px;text-align:left;border-bottom:1px solid #dbe4ed}}a{{color:#1764ae}}code{{overflow-wrap:anywhere}}.table{{overflow:auto}}.bar{{height:8px;background:#287cc3}}.metric{{font-size:28px;font-weight:bold}}</style>
<header><h1>Reach-RT / G2-03</h1><div>IPバイト計上・共通予算 — 映像・FEC・再送・応答・指令</div></header><main>
<section><div class="status">実通信検証 {"合格" if r['passed'] else "未合格"} — {sum(c['passed'] for c in r['cases'])} / {len(r['cases'])} ケース</div><p>IPv4 20 byte＋UDP 8 byte＋データグラム全体を、送信直前に一度だけ計上。全通信の合計と、上下それぞれのレート・バースト・累積上限を同時に検査します。</p><div class="metric">{first['attempted_ip_bytes']:,} IP byte</div><p>全種別ケース6秒の送信試行総量。上り {first['uplink_ip_bytes']:,} ＋ 下り {first['downlink_ip_bytes']:,}。モデル内で失われた送信も含みます。送信前の棄却は別記録です。</p></section>
<section class="table"><h2>どの通信が予算を消費したか</h2><table><thead><tr><th>方向</th><th>種別</th><th>送信試行IP byte</th><th>総量の割合</th></tr></thead><tbody>{''.join(classes)}</tbody></table></section>
<section class="table"><h2>上限・損失・停止の検証</h2><table><thead><tr><th>条件</th><th>判定</th><th>上り byte</th><th>下り byte</th><th>合計 byte</th><th>送信前棄却 byte</th><th>証拠</th></tr></thead><tbody>{''.join(cases)}</tbody></table><p>下り予算を使い切ると指令も送れなくなり、期限監視・制動で停止します。作業成功や方式の優位性を判定した表ではありません。</p></section>
<section><h2>記録の読み方</h2><p>予算不足では待ち行列を増やさず、送信前にそのパケットを棄却します。方向別上限は最低保証帯域ではありません。模擬回線のFIFO破棄・時刻損失・終了残留は別に記録し、予算を返却しません。中継出口の実装用UDP送信を二重計上せず、NICやEthernetの実測量とは区別します。</p><p>exe SHA-256：<code>{r['executable_sha256']}</code></p><a href="report.json">全検証JSON</a> / <a href="task_decision.json">G2-03完了判定</a> / <a href="auditor_fault_tests.json">監査器の改変検出</a> / <a href="evidence_audit.json">ソース対応・制動記録</a></section></main></html>'''
    (out/'index.html').write_text(page,encoding='utf-8');print(out/'index.html')


if __name__=='__main__':main()
