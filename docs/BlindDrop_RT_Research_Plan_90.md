# BlindDrop-RT 研究計画書

## 観測適応型攻撃下におけるリアルタイム・サイバーフィジカル通信の可用性保証

## 0. 研究構想

> **通信内容を暗号化するだけでは防げない観測適応型攻撃に対し、通信外形、回復資源、deadlineを統合制御することで、遠隔ロボット等のリアルタイム・サイバーフィジカル通信の知覚経路を、同一wire byte・遅延上限の下でどこまで可用に保てるかを明らかにする。**

本研究では、攻撃・防御・実アプリケーションを一体化した研究基盤 **BlindDrop-RT** を構築し、映像停止時間だけでなく、情報鮮度、制御継続性、遠隔タスク成功率まで実測する。

研究対象を「パケット秘匿」や「新しいFEC方式」に限定しない。固定通信Envelopeと秘密回復資源割当は、サイバーフィジカル・システムの知覚経路を守るための構成要素と位置づける。

```text
[観測適応型攻撃]
 暗号化通信のsize・timing・feedbackから重要部分を推定してdrop
             |
             v
[BlindDrop-RT]
 外形非依存Envelope + 秘密回復資源割当 + deadline制御
             |
             v
[End-to-End実証]
 packet -> decode -> display -> 操作継続 -> 物理タスク結果
             |
             v
[研究成果]
 攻撃成立条件、可用性改善、wire/latency代償、適用限界
```

## 1. 研究題目

### 日本語

**観測適応型攻撃下におけるリアルタイム・サイバーフィジカル通信の可用性保証**  
— **BlindDrop-RT：外形秘匿とDamage-Aware回復を統合したリアルタイム映像トランスポート**

### English

**Availability Assurance for Real-Time Cyber-Physical Communication under Observation-Adaptive Attacks**  
— **BlindDrop-RT: An Oblivious and Damage-Aware Transport for Resilient Real-Time Video**

## 2. 背景

遠隔ロボット、災害対応ドローン、遠隔点検、XR遠隔作業では、映像は単なるコンテンツではなく、人間または制御器が物理世界を認識するための知覚経路である。この経路が短時間停止するだけでも、操作中断、判断遅延、衝突、タスク失敗につながり得る。

しかし、映像payloadを暗号化しても、次の外形情報は残る。

- packet size
- packet timing
- burstとframe境界
- 平文header
- FEC量
- feedbackの時刻と量
- retransmission burst
- bitrate tier変化

圧縮映像ではpacketごとの重要度が等しくない。IDR、codec configuration、参照frameなどの消失は、同じpacket数のランダム消失より長いfreezeやdecoder同期喪失を起こし得る。したがって攻撃者は、暗号を解読しなくても、観測した外形と過去の攻撃結果から重要packetを推定し、限られた予算で知覚経路へ大きな可用性被害を与える可能性がある。

現在のRNVP v1は `frameId`、`chunkIndex`、keyframe、FEC、retransmit、codec typeなどを平文で持つため、選択攻撃の成立性を検証する実験基盤になる。

本研究が扱う問題は、次の三領域の交点にある。

```text
Systems Security
  暗号化後にも残る通信外形と能動的traffic analysis
        x
Real-Time Networking
  deadline、wire予算、混雑応答、損失回復
        x
Cyber-Physical / Multimedia Systems
  映像freeze、情報鮮度、操作継続性、タスク成功
```

## 3. 研究で解く問題

研究対象は一般的なDoS防止でも、暗号化単体でも、FEC単体でもない。対象は、暗号を破らずに通信外形を学習し、物理タスクへの被害が大きいpacketを因果的に選ぶ攻撃者である。

> **自然損失・blind攻撃・selective攻撃時の絶対QoE、情報鮮度、deadline、wire帯域、追加遅延を制約として守りながら、観測適応型攻撃者がblind攻撃を超えて得る追加の知覚・操作被害を最小化できるか。**

### 3.1 主実証シナリオ

主実証は、操作者が低遅延映像を見ながら遠隔ロボットを操作する閉ループ環境とする。物理ロボットが利用できない反復実験では、同一の通信・decoder・display経路を使用する再現可能なsimulationまたはrecord-and-replay環境を用いる。

中心タスクは、障害物回避を含む目標到達または対象物への接近・停止とする。cloud gamingと通常映像会議は副シナリオとし、一般化可能性の確認に使う。

### 3.2 保証対象

本研究でいう可用性保証は、無条件の連続動作を意味しない。事前に定めた攻撃、wire、遅延、自然損失の範囲内で、次を測定可能なguardrail以下に保つ設計・実証を意味する。

- 映像freezeとdecoder同期喪失
- captureから表示までの情報鮮度
- 操作入力から視覚確認までの閉ループ中断
- 安全停止を含むタスク失敗
- 自然損失・混雑時の非攻撃通信への悪影響

### 中心目的

```text
minimize:
  DA_visual(pi)  = D_selective(pi) - D_blind(pi)
  DA_control(pi) = C_selective(pi) - C_blind(pi)
  DA_task(pi)    = T_selective(pi) - T_blind(pi)

subject to:
  D_natural(pi), C_natural(pi), T_natural(pi)       <= respective guardrails
  D_blind(pi), C_blind(pi), T_blind(pi)             <= respective guardrails
  D_selective(pi), C_selective(pi), T_selective(pi) <= respective guardrails
  wire overhead   <= G_wire
  p95 extra delay <= L_p95
  max extra delay <= L_max
  AEAD/replay safety invariants are satisfied
```

`pi` はData、FEC、Repair、Dummy、送信順序を決める方策である。

`D` は映像被害、`C` は操作中断、`T` はタスク失敗を表す。恣意的な重み付き総合点へ早期に集約せず、三つの効果量とPareto関係を個別に報告する。初期の通信実験では `D` を中心指標とし、閉ループ実証で `C` と `T` を追加する。

damage advantageだけを小さくするためにblind時の映像や操作性まで悪化させる退化解は、絶対可用性制約で排除する。

## 4. 研究質問と学術的貢献

### RQ1：攻撃可能性

暗号化されたリアルタイム映像通信に対し、外形と過去の操作結果だけを利用する因果的攻撃者は、同一予算のblind損失より大きな映像・操作被害を安定して発生させられるか。

### RQ2：外形非依存性と実用性

映像重要度から外部scheduleを分離した固定Envelopeは、低遅延、wire効率、混雑応答性を維持しながら、未知の攻撃器によるdamage advantageを抑制できるか。

### RQ3：秘密領域内の回復最適化

外から見える通信量を変えずに、privateなcodec重要度、deadline、回復状態を用いてFEC・Repairを割り当てると、一様FECや公開UEPより最悪時のfreezeと閉ループ中断を小さくできるか。

### RQ4：サイバーフィジカル有効性

packet-levelの防御効果は、実decoder・display・操作者または制御器を通したときにも、情報鮮度、操作継続性、タスク成功率の改善として残るか。

### 期待する四つの貢献

1. **攻撃・評価モデル**：観測適応型選択攻撃と `damage advantage` により、暗号解読率ではなく物理タスクへ至る可用性被害を定量化する。
2. **通信アーキテクチャ**：外部scheduleを秘密状態から分離しつつ、deadlineと混雑応答を守るOblivious Real-Time Envelopeを設計する。
3. **回復最適化**：固定wire予算内で最悪被害を抑えるDORAを設計し、公開UEP・一様FECとの差を検証する。
4. **end-to-end artifact**：攻撃器、transport、decoder、display、遠隔タスクを接続した再現可能なBlindDrop Live Arenaを公開可能な形で整備する。

## 5. 新規性の境界

### 新規性として主張しないもの

- header暗号化
- 固定長packet
- paddingとDummy
- constant-rate pacing
- importance-aware FEC
- deadline-aware FEC・retransmission
- selective jammingをblind化する上位発想
- causal erasure attackerという形式

これらにはCryptex、IP-TFS、Pacer、NetShaper、Securitas、selective-jamming研究、video-aware UEP、SPFEC、Hairpin、causal adversarial channel研究などの先行研究がある。

### 新規性候補

1. **実映像damage advantageを中心に置くこと**

   分類AUCだけでなく、選択攻撃がblind攻撃を超えて増加させる実freeze被害を直接測る。

2. **private importanceを利用する秘密回復資源割当**

   codec依存、同期重要度、残りdeadlineを利用してFEC・Repairを割り当てる一方、その割当をsize、timing、feedback、retransmission burstへ露出させない。

3. **feedback-snooping attackerを含む閉ループ評価**

   過去の破棄結果と暗号化feedback外形を観測する攻撃者に対し、transport、FEC、decoder、displayまで含む実システムで評価する。

4. **通信被害と物理タスク被害を接続する評価**

   packet分類精度や映像品質だけで結論を出さず、同一の攻撃が情報鮮度、操作継続性、タスク成功へどう伝播するかを測る。

新規性は個々の暗号・padding・FEC機構ではなく、**攻撃モデル、外形非依存transport、秘密回復最適化、閉ループCPS評価を、一つの可用性問題として統合し、各要素の寄与を分離実証すること**に置く。

## 6. 観測適応型攻撃者

攻撃者はsender直後の単一観測点に存在する。

### 観測できるもの

- IP/UDP endpointと方向
- Outer Header
- wire packet number
- packet sizeと時刻
- 過去のpacket trace
- 過去に自分が行った操作
- 後から同じ観測点を通る暗号化feedbackの外形
- 公開rate tier

### 観測できないもの

- 現在時刻より未来のpacket
- 復号後のInner Header
- 映像内容
- 現在sessionの秘密割当
- 通信鍵
- 未通知のreceiver復号結果

### 操作

主研究ではpacket dropを扱う。遅延と並べ替えは副実験とする。

自然損失と攻撃損失の合成順序は次で固定する。

```text
sender output
  -> attacker decision at the declared observation point
  -> natural loss / delay trace
  -> receiver
```

主実験の攻撃者は過去の自然損失統計を知るが、将来のtrace実現値は知らない。副実験では順序を逆転させ、結論の感度を報告する。攻撃損失は自然損失とは別に計数し、総損失率も併記する。

```text
任意のW packet窓について:
  droppedPackets <= floor(rho * W)

rho in {0.02, 0.05}
consecutiveDrops <= b_max
```

1%と3%、遅延、並べ替え、混合攻撃はstretch条件とする。

### 攻撃集合

```text
A_blind(B) subseteq A_selective(B) subseteq A_oracle(B)
```

- `blind`：traceと独立な乱数または事前固定indexで攻撃
- `selective`：現在までの外部観測履歴から攻撃
- `oracle`：真のpacket roleとdamage labelも利用

## 7. 可用性被害指標

### 主指標：Freeze Time Ratio

表示frame間隔を `g_i`、通常frame間隔を `T_frame`、freeze判定閾値を `theta` とする。

```text
freezeDuration_i =
  g_i - T_frame,  if g_i > theta
  0,              otherwise

D(A, pi) = sum_i(freezeDuration_i) / sessionDuration
```

暫定値は次とし、50ms、100ms、150msで感度分析する。

```text
theta = max(100 ms, 3 * T_frame)
```

### 主たる差

```text
D_blind(pi)     = empirical maximum damage over blind ensemble
D_selective(pi) = empirical maximum damage over causal ensemble
D_oracle(pi)    = empirical maximum damage over oracle ensemble

DA_raw(pi) = D_selective(pi) - D_blind(pi)
```

`DA_raw` は映像被害に対する `DA_visual` の実装名として維持する。閉ループ実証では同様に次を報告する。

```text
DA_control(pi) = C_selective(pi) - C_blind(pi)
DA_task(pi)    = T_selective(pi) - T_blind(pi)
```

### 副指標：NDA

```text
NDA(pi) =
  (D_selective(pi) - D_blind(pi))
  / (D_oracle(pi) - D_blind(pi) + epsilon)
```

NDAは分母が小さいと不安定になるため副指標とする。主結論は単位が明確な `D_selective` と `DA_raw` に基づける。

### その他の副指標

- 500ms以上のfreeze発生率
- 最大連続freeze時間
- decoder同期回復時間
- undecodable frame率
- deadline内goodput
- VMAF / SSIM / PSNR
- p95 capture-to-display delay
- Age of Information相当の表示frame鮮度
- 連続して視覚確認できない操作時間
- 安全停止回数
- 遠隔タスク完了率と完了時間
- 衝突・経路逸脱等のタスク固有失敗率
- wire byte
- CPU、memory、Dummy比率

映像指標は全通信実験で必須とし、操作・タスク指標は閉ループ実証で必須とする。主結論は、packet loss率や攻撃器AUCだけでは支持しない。

## 8. BlindDrop-RTの構成

### 8.1 RNVP v2 Cell

すべてのUDP payloadを同じcell sizeにする。中心実験は1200 byteとし、800、1000 byteを感度分析に使う。

Outer Header:

- protocol version
- connection ID
- wire packet number
- key phase

AEADで保護するInner Header:

- Data / FEC / Repair / Dummy
- frameId
- chunkIndex / chunkCount
- keyframe / codec configuration
- codecType
- deadline class
- FEC generation
- Repair情報
- 実payload長

研究実験では既存AEAD実装と事前共有鍵を使用する。完全な公開鍵handshakeは中心研究の対象外とする。

必須安全条件:

- nonce再利用0
- replay受理0
- 未認証packet受理0
- 未認証feedbackによる状態変更0
- packet number wrap前にrekey

### 8.2 Oblivious Real-Time Envelope

通信を中心値20msのMicro-Epochに分け、各Epochのslot数と送信時刻を固定する。

```text
x_e = (data_e, fec_e, repair_e, dummy_e)

data_e + fec_e + repair_e + dummy_e = N_e
```

映像重要度、keyframe、Repair量によって `N_e` を変えない。

rate tierはraw wire delivery、RTT、queue delayなどの公開状態だけから決める。同じwire履歴なら、異なる映像内容でも同じ外部scheduleになることをproperty testで検証する。

固定Envelopeを無制限なconstant-rate tunnelにはしない。各tierについて、送信率、encoder入力上限、回復用予約slot、最大queue滞留時間を事前に定める。

- encoder入力が容量を超える場合は、送信前deadline dropまたは公開規則による品質低下を行う
- privateな映像重要度や内部backlogだけを理由に外部rate tierを上げない
- RTT、明示的輻輳通知、delivery rate、公開queue状態に応じて送信率を低下させる
- feedback断、継続損失、queue遅延超過時は次の制御機会でrate低下または安全停止する
- Dummy枯渇、Data overflow、deadline前送信不能、tier遷移回数を必ず記録する

攻撃耐性とInternet上の混雑安全性を別々の成功条件として評価する。

### 8.3 DORA-v0

各symbol `j` に対して、onlineで利用できる次の状態を持つ。

```text
s_j = {
  codecRole,
  descendantsWithinHorizon,
  remainingDeadline,
  transmittedCopies,
  recoveredState,
  feasibleRepairRounds
}
```

重要度 `q_j` はtraining映像だけで係数を決め、test映像では固定する。

```text
proxyLoss(a_e, lossScenario) =
  sum_j q_j * P(j is unrecoverable before deadline)
  + lambda_late * predictedLateCells
  + lambda_dummy * dummyCells

a_e* = arg min_{a_e in feasibleActions}
       max_{lossScenario in trainingScenarios}
       proxyLoss(a_e, lossScenario)
```

DORA-v0が最小化するのはcodec構造に基づく代理損失である。実際のfreeze改善は実decoderとdisplayで別に評価する。

実装はbounded enumerationまたは動的計画法とし、20ms以内に終了しない場合は安全な一様FEC方策へfallbackする。

### 8.4 Feedback

- 20～40ms境界で固定長・固定個数のfeedbackを送る
- lossがなくてもcover feedbackを送る
- missing chunk情報を暗号化する
- Repair発生時も外部packet数を増やさない
- feedback連続欠落時はsecret-independentにrateを低下または停止する

### 8.5 Placement

配置方式は次の三つを比較する。

1. interleavingなし
2. 公開ランダムinterleaving
3. 秘密Permutation

秘密Permutationは主貢献としない。公開interleavingを有意に上回った場合のみ有効な補助機構として残す。

### 8.6 End-to-End CPS Adapter

transportの評価を映像再生で終わらせないため、次の時刻を共通IDで接続する。

```text
capture -> encode -> send -> receive -> decode -> display
                                      -> operator/control action
                                      -> physical/simulated task event
```

これにより、packet損失がfreezeへ、freezeが操作中断へ、操作中断がタスク結果へ伝播する過程を再生可能にする。transportはロボット固有制御から分離し、映像以外の緊急停止channelを攻撃対象外として残す。

## 9. 研究仮説

### H0：攻撃成立性

事前登録した主条件において、RNVP v1に対する観測適応型選択攻撃は、同一予算の最良blind攻撃より、実用上意味のあるfreeze被害を追加する。

### H1：DORAの中心効果

同一codec出力、wire byte、cell size、Epoch数、平均FEC量、攻撃予算において、DORA-v0は最適調整された一様FECより `D_selective` と `DA_raw` を低下させる。

### H2：秘密化の効果

公開UEPと秘密DORAへ同じ重要度・FEC割当を与えたとき、割当を外部へ露出しない秘密DORAの方が因果的攻撃者の `DA_raw` を小さくする。

### H3：Feedback秘匿

固定feedbackはactive probe攻撃のAUCだけでなく、実freeze damageを低下させる。

### H4：制約適合性

探索目標として、次を同時に目指す。

- g4比平均追加wire byte 15%以下
- p95追加遅延40ms以下
- 2%攻撃時NDA 0.10以下

これらは既知の達成値ではなくpilot前の探索目標である。単一点だけでなく、`D_selective`、`DA_raw`、wire overhead、latencyのPareto frontierを報告する。

### H5：閉ループ有効性

同一の遠隔タスク、wire・latency予算、攻撃予算において、BlindDrop-RTの映像被害低下は、情報鮮度、操作継続性またはタスク成功率の少なくとも一つの事前登録指標でも改善として残り、他の安全指標を許容範囲以上に悪化させない。

## 10. 比較方式

### 必須baseline

1. RNVP v1
2. RNVP v1 + Inner Header AEAD
3. 固定Cell + 固定Envelope + 一様FEC
4. 固定Envelope + 公開priority-aware FEC
5. 固定Envelope + deadline-aware Data/FEC/Repair
6. 固定Envelope + DORA-v0、feedback秘匿なし
7. BlindDrop-RT

RNVP v1は攻撃成立性のsanity baselineとする。論文の主比較相手は3～5とする。

### 既存研究との対応

| RNVP baseline | 対応する既存研究上の考え方 |
|---|---|
| 固定Envelope | IP-TFS / Pacer型のsecret-independent shape |
| 公開priority-aware FEC | video-aware UEP / SPFEC型 |
| deadline-aware回復 | Hairpin型 |
| insertionによる外形難読化 | Securitas型 |

原研究の完全再実装ではない場合、再現した性質、再現しない性質、parameter対応を明記する。

### 公平性

- 同じ事前encode済み映像を使う
- 同じ有効Data量を使う
- 同じ平均wire byte上限を使う
- 同じlatency上限を使う
- 同じattack seedを使う
- 各方式のparameter探索回数を揃える
- defenseごとに攻撃器を再学習する
- 各攻撃器のtraining data、query回数、学習時間またはcompute budgetを揃える
- 攻撃器のhyperparameter探索範囲と停止条件を事前に固定する
- CPS実証では同じ初期状態、経路、操作規則または操作者割当を使う

## 11. Oracle設計

oracleが弱いためにNDAを誤評価しないよう、三段階用意する。

1. **Header Oracle**

   keyframe、FEC、Repair、frame/chunk情報を知る。

2. **Marginal-Damage Oracle**

   単一symbolを除いたcounterfactual decodeから得たdamage labelを知る。

3. **Combinatorial Oracle**

   rolling window内の上位候補に対して、greedy再評価またはbeam searchで複数packetの相互作用を探索する。

NDAの参照には3を使用する。ただし情報理論的upper boundとは呼ばず、実装した中で最も強い参照攻撃と表現する。

## 12. 実験設計

### Dataset分割

```text
pilot set:
  閾値、分散、実行可能範囲の推定だけに使う

training set:
  q_j、DORA parameter、attack model、baseline parameterを決める

test set:
  最終結果だけに使い、parameterを変更しない
```

映像単位、network trace単位で分離する。同じ映像の別区間をtrainingとtestへ混在させない。

### 実験単位

```text
video x networkTrace x attackSeed
```

方式間で同じ組をpaired comparisonする。

閉ループ実証では次を実験単位とし、操作者を使う場合は順序効果をcounterbalanceする。

```text
taskScenario x initialState x networkTrace x attackSeed x operator/controlPolicy
```

### 変動軸

- scene motion
- GOP / IDR間隔
- bitrate
- resolution
- 自然損失率
- burst長
- RTT
- queue delay
- 操作速度と安全停止閾値
- タスク難度と障害物配置
- 自動制御、記録済み入力、人間操作者

### 統計

- pilot分散からsample sizeを決める
- 最小実用差 `delta_DA` をtest前に固定する
- video/network trace単位のcluster-aware paired bootstrapを使う
- 平均、median、p95、95% confidence intervalを報告する
- H0～H4の確認分析には多重比較補正を行う
- 探索分析と確認分析を分ける

Gate 1では、事前登録した主条件において次を要求する。

```text
lower95CI(DA_raw) > delta_DA
```

Gate 3では、`D_selective` と `DA_raw` の一方に事前登録した優越性を要求し、もう一方には非劣性marginを要求する。両方で優越した場合をstrong successとして別に報告する。

閉ループタスクの二値成功率にはpairedまたは階層的な二項モデルを用い、映像・trace・操作者を独立なpacket試行として扱わない。

### Holdout attack

開発中に使わなかったattack familyを一つ以上、最終testだけで使用する。方式ごとに再学習し、BlindDropだけ未知attackで評価する不公平を避ける。

### 12.1 三段階評価

1. **Transport bench**：事前encode映像と再生network traceで、大規模なpaired反復を行う。
2. **End-to-end media bench**：実capture、encode、decode、displayを通し、latencyとfreezeを測る。
3. **BlindDrop Live Arena**：遠隔ロボットまたは同一制御経路のsimulationで、操作継続性とタスク結果を測る。

第3段階だけで統計的主張を成立させず、第1・第2段階で原因を分離した上で外的妥当性を確認する。

## 13. 成功判定

### Gate 1：攻撃が実在するか

事前登録した主条件で、causal selective attackについて `lower95CI(DA_raw) > delta_DA` を満たすこと。

成立しない場合は防御を拡張せず、選択攻撃が成立しない条件を研究結果にする。

### Gate 2：固定Envelopeが現実的か

固定Cellと固定feedbackが、pilot後に固定したwire overheadとlatency guardrailを満たすこと。

満たさない場合はDORA前にEnvelope設計を見直す。

### Gate 3：DORAに価値があるか

同一wire・latency予算で、DORA-v0が一様FECより `D_selective` または `DA_raw` の一方を事前登録した最小実用差以上低下させ、もう一方で事前登録した非劣性marginを満たすこと。両指標の優越をstrong successとする。

満たさない場合、RLや大規模学習で結果を追わず、importance modelまたは中心仮説を再検討する。

### Gate 4：CPS上の意味が残るか

End-to-end media benchで得た改善が、BlindDrop Live Arenaにおいて情報鮮度、操作継続時間、タスク成功率の少なくとも一つで事前登録した最小実用差以上残り、他の安全指標と自然損失時性能で非劣性を満たすこと。

満たさない場合、主張をリアルタイム映像transportまでに限定し、サイバーフィジカル可用性保証とは結論しない。

### 最終成功条件

次をすべて満たす。

1. H0が事前登録条件で成立する
2. DORAが強い一様FECまたは公開UEPを同一wire予算で上回る
3. 自然損失・blind攻撃時のQoEを許容範囲より悪化させない
4. holdout映像・network trace・attackerでも効果が残る
5. wire overheadと追加遅延を実測してPareto優越を示す
6. 全安全性不変条件を満たす
7. 混雑応答、encoder overload、Dummy枯渇時の挙動を実測し、公開したguardrailを満たす
8. 閉ループCPS指標でGate 4を満たす。満たさない場合は研究題目と結論の適用範囲を縮小する

## 14. 最小実装範囲

中心研究を次に限定する。

| 項目 | 中心範囲 |
|---|---|
| codec | H.264 |
| FEC | 現在のXOR g4 |
| 主攻撃 | packet drop |
| 攻撃予算 | 2%、5% |
| Cell | 1200 byte中心 |
| Epoch | 20ms中心 |
| DORA | bounded enumerationまたはDP |
| 暗号 | 既存AEAD＋実験用PSK |
| 観測点 | sender直後の単一egress点 |
| 主実証 | 遠隔ロボットの目標到達または再現可能な同等simulation |
| 安全channel | 映像transportと独立した緊急停止channel |

stretch goalへ送るもの:

- 1%、3%攻撃
- 遅延・並べ替え・混合攻撃
- Reed-Solomon / Raptor / streaming code
- online bandit / reinforcement learning
- 複数codec
- 複数観測点
- NAT traversal
- 公開鍵handshake
- 無線PHY jamming

## 15. 実装順序

### Phase 0：測定基盤

1. freeze定義とevent logを固定
2. pilot/training/test splitを固定
3. paired experiment runnerを作る
4. RNVP v1の再現性を確認

### Phase 1：攻撃benchmark

5. blind attack
6. learned causal attack
7. Header Oracle
8. Marginal/Combinatorial Oracle
9. DA_rawとNDAの計算

### Phase 2：固定Envelope

10. RNVP v2 CellとAEAD
11. fixed-size packet
12. 20ms slot scheduler
13. fixed feedback
14. congestion circuit breaker

### Phase 3：DORA-v0

15. private importance計算
16. deadline feasibility
17. bounded action search
18. 一様FEC fallback
19. 公開UEP・一様FECとのA/B

### Phase 4：最終評価

20. ablation
21. Pareto frontier
22. holdout evaluation
23. artifact manifest
24. BlindDrop Live Arena

### Phase 5：適用範囲と公開

25. CPSタスク評価とGate 4判定
26. 自然損失・混雑・overload安全性評価
27. artifact匿名化と再現手順
28. 責任ある攻撃器公開範囲の決定

## 16. 成果物

1. 観測適応型選択攻撃benchmarkとdamage-advantage評価法
2. Oblivious Real-Time Envelopeを備えたRNVP v2
3. 固定wire予算内で動作するDORA-v0
4. 再現可能な映像・network・attack dataset
5. transportから物理タスクまで追跡できる評価runner、manifest、集計コード
6. 攻撃・防御・操作結果を同時提示するBlindDrop Live Arena
7. 成立条件、限界条件、negative resultを含む実証報告

## 17. BlindDrop Live Arena

遠隔ロボットまたは同一制御経路のsimulationを動作させ、三画面を同時表示する。

- 左：RNVP v1または強い一様FEC baseline
- 中央：攻撃者の観測と現在の攻撃判断
- 右：BlindDrop-RT

常に次を表示する。

- 累積wire byte
- attack budgetと実破棄数
- freeze time ratio
- 最大freeze
- p95追加遅延
- 現在runのDA_raw
- capture-to-display情報鮮度
- 操作中断時間とタスク進行状況
- 安全停止、衝突、経路逸脱

最良runだけを表示しない。BlindDropが不利なscene、攻撃が成立しないscene、帯域制約を満たせない条件も切り替えて示す。

## 18. 主要リスク

| リスク | 判定 | 対応 |
|---|---|---|
| 選択攻撃がblindより強くならない | 研究仮説不成立 | Gate 1で停止しnegative result化 |
| 固定Envelopeの帯域が大き過ぎる | 高 | Gate 2、cell/tier感度分析、Pareto評価 |
| DORAが一様FECを上回らない | 中～高 | importance modelを検証し、RLで結果を追わない |
| 代理損失がfreezeと相関しない | 中 | 実decode評価、ablation、相関報告 |
| feedback集約でRepairが遅れる | 高 | 20/30/40ms感度分析 |
| secret permutationに効果がない | 中 | 公開interleaving比較後に削除可能 |
| 実装範囲が大き過ぎる | 高 | H.264/XOR/drop-onlyへ中心範囲を固定 |
| 映像改善がタスク改善へつながらない | 中～高 | Gate 4で主張範囲を縮小し、伝播しない理由を分析 |
| constant-rateが混雑を悪化させる | 高 | 公開信号によるtier低下、circuit breaker、RFC 8085適合性評価 |
| タイトルだけがCPSで実験が映像のみ | 高 | Live Arena、タスク指標、Gate 4を必須化 |

## 19. 近接研究と研究上の位置

| 研究領域 | 近接研究が示したこと | 本研究で検証する未充足部分 |
|---|---|---|
| Header confidentiality | [Cryptex / RFC 9335](https://www.rfc-editor.org/rfc/rfc9335.html) | headerを隠した後にも残るsize・timing・feedbackと実QoE被害 |
| Traffic Flow Confidentiality | [IP-TFS / RFC 9347](https://www.rfc-editor.org/rfc/rfc9347.html) | 低遅延映像deadline、秘密回復割当、CPSタスクとの統合 |
| Secret-independent shaping | [Pacer, USENIX Security 2022](https://www.usenix.org/conference/usenixsecurity22/presentation/mehta)、[NetShaper, USENIX Security 2024](https://www.usenix.org/conference/usenixsecurity24/presentation/sabzi) | 能動的drop攻撃下の映像・操作可用性と固定wire内のUEP |
| Encrypted traffic obfuscation | [Securitas, NSDI 2026](https://www.usenix.org/conference/nsdi26/presentation/xie-guorui) | passiveな推定精度だけでなく、active attackが生むend-to-end damage |
| Selective jamming | [Proaño and Lazos, ICC 2010](https://experts.arizona.edu/en/publications/selective-jamming-attacks-in-wireless-networks/) | UDP映像・feedback・実decoderを含む因果的学習攻撃と防御 |
| Deadline-aware recovery | [Hairpin, NSDI 2024](https://www.usenix.org/conference/nsdi24/presentation/meng) | 回復優先度そのものを外形へ露出させない防御 |
| Fine-grained video FEC | [Tooth, NSDI 2025](https://www.usenix.org/conference/nsdi25/presentation/an) | adversarial observation下でのprivate importanceと同一wire比較 |
| Loss-resilient real-time video | [Morphe, NSDI 2026](https://www.usenix.org/conference/nsdi26/presentation/gong) | 自然損失だけでなく、重要部分を狙う観測適応型攻撃 |
| QoE-aware multi-connectivity | [QCON, NSDI 2026](https://www.usenix.org/conference/nsdi26/presentation/lee) | multi-pathとは独立した外形漏洩・選択攻撃への耐性 |
| Priority-aware conferencing FEC | [SPFEC, 2026 preprint](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=6880455) | 2026年8月時点では査読済み論文と断定せずpreprintとして扱い、公開priorityが攻撃面を作るかを比較する |

この位置づけから、本研究を純粋暗号研究とは呼ばない。主分野を **networked systems / systems security**、副分野を **multimedia systems / cyber-physical systems** とする。

## 20. 倫理・デュアルユース・安全

選択攻撃器は防御評価に必要だが、第三者通信への妨害にも転用可能である。実験と公開は次の原則に従う。

- 攻撃実験は所有・管理下の閉鎖network、emulator、simulation内に限定する
- 第三者endpoint、公共network、無線妨害を対象にしない
- datasetから個人映像、endpoint、鍵、操作ログの識別情報を除く
- 人間操作者を対象とする場合は、所属機関の倫理手続、同意、途中退出、安全停止を準備する
- 物理ロボットには独立した緊急停止、速度制限、立入制限を設ける
- 攻撃器公開時は実験用trace interfaceを標準とし、live traffic妨害機能の公開範囲を別途審査する
- 防御が保証する攻撃予算、観測点、codec、遅延範囲を明示し、範囲外の安全を主張しない

## 最終要約

本研究の主語は、FEC、padding、header暗号化ではない。**通信内容を暗号化するだけでは防げない観測適応型攻撃下で、リアルタイム・サイバーフィジカル通信の知覚経路をどこまで可用に保てるか**である。

BlindDrop-RTは、この問いを検証するためのend-to-end研究システムである。privateな映像重要度を回復資源の割当に利用しながら、その割当を通信外形へ出さず、同一wire・latency予算の下で、未知の因果的攻撃者によるfreeze、情報鮮度低下、操作中断、タスク失敗をどこまで減らせるかを測る。

研究は、攻撃成立性、Envelope実行可能性、DORAの追加価値、CPS上の有効性という四つのGateで判定する。最後のGateを満たさない場合は主張を映像transportへ縮小する。どのGateで仮説が成立しなくても、成立条件と限界を再現可能な研究結果として残す。
