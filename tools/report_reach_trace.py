"""Standalone record viewer for G2-02; reads only recorded results."""
import argparse
import html
import json
import os
from pathlib import Path


def main():
    parser=argparse.ArgumentParser();parser.add_argument('report',type=Path);args=parser.parse_args()
    report=json.loads(args.report.read_text(encoding='utf-8'));folder=args.report.resolve().parent
    def href(path):return html.escape(os.path.relpath(path,folder).replace('\\','/'))
    names={'paired_T1':'同一トレース：T1','paired_T2':'同一トレース：T2','command_blackout':'指令の損失区間','delay_reorder':'遅延変更・順序逆転','propagation_close':'終了時の伝搬中残留'}
    rows=[]
    for c in report['cases']:
        up,down=c['uplink'],c['downlink'];session=Path(c['session'])
        rows.append(f'<tr><td>{names[c["name"]]}</td><td>{"合格" if c["passed"] else "未合格"}</td><td>{up["offered_packets"]}</td><td>{up["trace_dropped_packets"]}</td><td>{down["trace_dropped_packets"]}</td><td>{up["tail_dropped_packets"] + down["tail_dropped_packets"]}</td><td>{up["propagation_cancelled_packets"] + down["propagation_cancelled_packets"]}</td><td><a href="{href(session/"uplink_link.csv")}">上り記録</a> / <a href="{href(session/"downlink_link.csv")}">下り記録</a></td></tr>')
    charts=[]
    for c in report['cases'][:2]:
        session=Path(c['session']);trace=json.loads((session/'uplink_trace.json').read_text());rect=[]
        for i,p in enumerate(trace['impairment'][:-1]):
            start=p['at_us']/6000000*900;end=trace['impairment'][i+1]['at_us']/6000000*900
            rect.append(f'<rect x="{start}" y="0" width="{end-start}" height="24" fill="{"#bb4040" if p["drop"] else "#2e8c73"}"><title>{p["at_us"]/1000000:.1f}秒：{"損失" if p["drop"] else "送達"}</title></rect>')
        charts.append(f'<p>{names[c["name"]]} ／ 上り入力 {c["uplink"]["offered_packets"]} パケット</p><svg role="img" aria-label="上りの時刻基準損失区間" viewBox="0 0 900 24">{"".join(rect)}</svg>')
    page=f'''<!doctype html><html lang="ja"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Reach-RT G2-02 検証結果</title>
<style>body{{margin:0;background:#eef3f8;color:#193149;font:16px/1.75 "Yu Gothic UI",sans-serif}}header{{background:#13273f;color:white;padding:28px}}main{{max-width:1150px;margin:auto;padding:24px}}section{{background:white;padding:24px;margin:0 0 20px;border-radius:10px}}h1{{margin:0}}.status{{font-size:26px;color:#08785c}}table{{width:100%;border-collapse:collapse}}th,td{{padding:10px;text-align:left;border-bottom:1px solid #dbe4ed}}a{{color:#1764ae}}code{{overflow-wrap:anywhere}}.table{{overflow:auto}}svg{{width:100%;height:24px}}.axis{{display:flex;justify-content:space-between}}</style>
<header><h1>Reach-RT / G2-02</h1><div>時刻基準の障害トレース・乱数分離</div></header><main>
<section><div class="status">実通信検証 {"合格" if report['passed'] else "未合格"} — {sum(c['passed'] for c in report['cases'])} / {len(report['cases'])} ケース</div><p>送信数に関係なく、同じ経過時刻に同じ容量・損失区間・遅延を与えます。直列化完了のモデル時刻で状態を参照し、OSの実行遅れは別記録です。</p><p>方式の優劣を比較した結果ではありません。T1/T2は、異なるパケット列でも外生条件が変わらないことを確認する入力です。</p></section>
<section><h2>送信数が違っても、障害の時間帯は同じ</h2>{''.join(charts)}<div class="axis"><span>0秒</span><span>3秒</span><span>6秒</span></div><p>緑＝送達区間、赤＝損失区間。上下両方向の容量・損失・遅延を含むトレースのハッシュ一致も確認済みです。キュー滞留と実際の損失数は送信内容・時刻によって変わります。</p><p>固定トレースID：<code>{report['bundle_trace_id']}</code></p><a href="paired_trace/manifest.json">生成条件・方向／現象別の乱数キー</a> / <a href="paired_trace/link.json">固定トレース本体</a></section>
<section class="table"><h2>全ケースと原記録</h2><table><thead><tr><th>条件</th><th>判定</th><th>上り入力</th><th>上り時刻損失</th><th>下り時刻損失</th><th>キュー破棄</th><th>終了時伝搬残留</th><th>証拠</th></tr></thead><tbody>{''.join(rows)}</tbody></table></section>
<section><h2>前提と限界</h2><p>合成した時間セルの障害であり、実5Gの測定分布やパケットごとの独立損失を表すものではありません。損失区間の割合はパケット損失率と一致するとは限りません。共通IP予算・全ACK／状態通知は後続工程です。</p><p>実行exe SHA-256：<code>{report['executable_sha256']}</code></p><a href="report.json">全結果JSON</a> / <a href="task_decision.json">G2-02完了判定</a> / <a href="trace_auditor_faults.json">監査器の改変検出</a></section></main></html>'''
    (folder/'index.html').write_text(page,encoding='utf-8');print(folder/'index.html')


if __name__=='__main__':main()
