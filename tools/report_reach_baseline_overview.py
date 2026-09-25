"""Put qualified paired results first; keep all training/failure tables below."""
import argparse
import html
import json
from pathlib import Path


def main():
    p=argparse.ArgumentParser();p.add_argument('folder',type=Path);args=p.parse_args();out=args.folder
    report=json.loads((out/'qualified_report.json').read_text(encoding='utf-8'));gate=json.loads((out/'task_decision.json').read_text(encoding='utf-8'))
    if not report['passed'] or not gate['passed']:raise ValueError('qualification is incomplete')
    validation=[r for r in report['trials'] if r['phase']=='validation'];cards=[]
    for mode in ('B0','B1'):
        entries=[r for r in validation if r['mode']==mode];count=sum(r['integration']['motion']['outcome']=='success' for r in entries)
        amount=sum(r['budget']['summary']['attempted_ip_bytes'] for r in entries)
        p95=[r['baseline']['decision_us_p95']/1000 for r in entries]
        cards.append(f'<article style="background:#f0f5f9;padding:22px;border-radius:10px;flex:1;min-width:230px"><h3>{mode}</h3><p style="font-size:38px;margin:8px 0;font-weight:700">{count} / {len(entries)} <span style="font-size:16px">条件で作業成功</span></p><p>全4試行の通信量 <b>{amount/1e6:.3f} MB</b>（IP）</p><p>各試行の判断時間 p95：{min(p95):.3f}～{max(p95):.3f} ms</p></article>')
    names=dict(valid_T1_normal='T1 / 正常',valid_T2_normal='T2 / 正常（追加の対応試験）',valid_T1_mixed='T1 / 複合障害',valid_T2_small_queue='T2 / 8 KiBキュー')
    lines=[]
    for name in names:
        cells=[names[name]]
        for mode in ('B0','B1'):
            r=next(r for r in validation if r['case']==name and r['mode']==mode);m=r['integration']['motion']
            outcome={'success':'成功','timeout':'時間切れ','collision':'衝突','out_of_bounds':'領域外'}.get(m['outcome'],m['outcome'])
            if m['outcome']=='success':outcome+=f" / {m['terminal']['simulation_s']:.2f}秒"
            cells.extend([outcome,f"{r['budget']['summary']['attempted_ip_bytes']/1e6:.3f} MB"])
        lines.append('<tr>'+''.join('<td>'+html.escape(x)+'</td>' for x in cells)+'</tr>')
    section='<section id="qualification-overview"><h2>調整後の対応比較：同じ予算・同じ障害トレース</h2><div style="display:flex;flex-wrap:wrap;gap:18px">'+''.join(cards)+'</div><div class="scroll"><table><tr><th>条件</th><th>B0 作業結果</th><th>B0 通信量</th><th>B1 作業結果</th><th>B1 通信量</th></tr>'+''.join(lines)+'</table></div><p>各方式・各条件1回、60秒窓。失敗試行も含む実測です。完了秒は物理シミュレーション時刻（0.01秒単位）。成功率の推定や統計的優位性を示すものではありません。1 MB = 1,000,000 IP byte。</p><p>G3-01の実装・初期調整は完了。G3全体の研究成立判定はこれからです。</p></section>'
    page=out/'index.html';content=page.read_text(encoding='utf-8')
    if 'id="qualification-overview"' in content:raise ValueError('overview already rendered')
    notice='<section><h2>無効試行と追加試験を区別して記録</h2><p>当初20試行のうち正常T2のB0が撮影周期の取りこぼしで無効終了しました。元の結果は保持し、同じコード・係数・条件のB0/B1を追加の一組として確認しました。上の対応比較の正常T2は追加の両試行を使用し、元の有効B1は補足データとして保持しています。</p><p>最終候補版は計22試行、有効な60秒窓21件・無効1件。下の全試行表は当初20件の記録で、無効行も残しています。</p><p><a href="trial_disposition.json">全試行の扱い</a> / <a href="qualified_report.json">対応比較の集約</a> / <a href="../supplement_01/plan.json">追加試験の事前計画</a> / <a href="../supplement_01/report.json">追加2件の全監査</a></p></section>'
    content=content.replace('G3-01 計測・判定中','G3-01 実装・初期調整完了（無効試行1件を別記）')
    page.write_text(content.replace('<section>',section+notice+'<section>',1),encoding='utf-8')
    print(page)


if __name__=='__main__':main()
