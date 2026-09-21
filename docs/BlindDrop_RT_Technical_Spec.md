# BlindDrop-RT 技術・研究実験仕様書

## 0. 文書管理

| 項目 | 値 |
|---|---|
| 文書名 | BlindDrop-RT Technical and Experimental Specification |
| 文書版 | 0.1.0 |
| 状態 | Implementation Baseline |
| 対象実装 | TR2 / RNVP v1を基盤とするRNVP v2実験系 |
| 主対象 | H.264リアルタイム映像、UDP、選択的packet drop |
| 研究計画 | `docs/BlindDrop_RT_Research_Plan_90.md` |
| 既存仕様 | `docs/RNVP_v1_Protocol_Spec.md` |

本書は研究構想を実装可能な要求、protocol、component、実験手順、合否条件へ変換したものである。本書に記載する性能値のうち、`TARGET` は未達成の研究目標であり、実測済み性能を意味しない。

### 0.1 規定語

- `MUST`：満たさなければ当該phaseを完了としない。
- `SHOULD`：原則として満たす。満たさない場合は理由をmanifestへ残す。
- `MAY`：任意実装。
- `FIXED`：確認実験中に変更してはならない。
- `TUNABLE`：pilotまたはtraining setだけで調整できる。
- `PILOT-LOCK`：pilot後、test開始前に値を固定する。
- `TARGET`：達成を検証する目標値。既知の達成値ではない。

## 1. 目的

BlindDrop-RTは、暗号化通信の外形を観測して重要packetを因果的に選択する攻撃者に対し、次を同時に実現できるかを検証する研究システムである。

1. packet role、frame境界、FEC量、Repair発生を外部traceから推定しにくくする。
2. 固定されたwire budget内部で、重要度とdeadlineに応じてData、FEC、Repairを秘密に割り当てる。
3. 自然損失と混雑に応答し、無制限なconstant-rate送信を行わない。
4. 同一wire byte、遅延上限、攻撃予算で既存方式と比較する。
5. packet lossから実decoder、display、遠隔タスクまでの被害伝播を測る。

### 1.1 中心仮説

同一の映像入力、wire budget、latency guardrail、攻撃予算において、BlindDrop-RTは強く調整した一様FECおよび公開UEPより、次を低減する。

```text
DA_visual  = D_selective - D_blind
DA_control = C_selective - C_blind
DA_task    = T_selective - T_blind
```

### 1.2 非目標

次はv0.1の中心範囲に含めない。

- 独自暗号方式の設計
- 公開鍵handshake、証明書、PKI
- 一般Internet全体に対する無条件の可用性保証
- 無線PHY jamming
- 複数観測点を共謀させる攻撃
- Reed-Solomon、Raptor、streaming code
- reinforcement learningによるallocation
- 映像以外の緊急停止channelの秘匿
- すべてのcodec、CPS、network条件への一般化

## 2. 現行基盤と追加範囲

### 2.1 現行RNVP v1で再利用する機能

- H.264 encode/decode
- frame/chunk packetization
- XOR group FEC
- deadline NACK
- bounded selective retransmission
- TransportFeedback
- PacketPacer
- JitterBuffer
- AdaptiveStreamingController
- NetworkConditionSimulator
- NetworkExperimentRunner / Reporter / Replay
- NetworkCsvLogger

### 2.2 新規実装する機能

| ID | Component | 責務 |
|---|---|---|
| BD-01 | RNVP v2 Cell Codec | 固定長cellのencode/decode |
| BD-02 | Session Crypto | AEAD、key derivation、nonce、replay防止 |
| BD-03 | Oblivious Envelope | Epoch、slot、Dummy、公開rate tier |
| BD-04 | DORA-v0 | private importanceに基づく回復資源割当 |
| BD-05 | Oblivious Feedback | 固定周期・固定長feedback |
| BD-06 | Selective Attack Simulator | blind、causal、oracle攻撃 |
| BD-07 | Damage Instrumentation | cell、frame、display、taskの共通計測 |
| BD-08 | CPS Experiment Adapter | 操作・物理task eventとの接続 |
| BD-09 | BlindDrop Reporter | DA、NDA、Pareto、Gate判定 |

### 2.3 互換性方針

- RNVP v1 wire formatを変更してはならない。
- RNVP v2はfeature flagで有効化する。
- v1とv2は同一binaryから選択できなければならない。
- 同一run内でv1とv2を自動fallbackさせてはならない。比較条件が不明になるため、versionはrun開始時に固定する。
- v2 packetをv1 receiverが受信した場合は安全に破棄する。

## 3. システム構成

```text
Capture
  -> H.264 Encoder
  -> Media Symbolizer
  -> DORA-v0
  -> Envelope Scheduler
  -> RNVP v2 Cell + AEAD
  -> Attack Decision Point
  -> Natural Loss / Delay Simulator
  -> UDP Receiver
  -> AEAD + Replay Check
  -> FEC / Repair Recovery
  -> Frame Reassembler
  -> Decoder
  -> Display
  -> CPS Task Adapter
```

### 3.1 信頼境界

- `Media Symbolizer`から`AEAD`までをsender trusted domainとする。
- `AEAD verification`以降をreceiver trusted domainとする。
- UDP path、Attack Simulator、Natural Loss Simulatorはuntrusted domainとする。
- 実験用truth labelは評価loggerだけが参照できる。攻撃器へ渡してはならない。
- Oracle attackerだけは明示的にtruth interfaceを使用できる。

## 4. Threat Model

### 4.1 観測点

主実験の攻撃者はsenderのAEAD処理後、自然損失適用前の単一egress点に存在する。

### 4.2 観測可能情報

- IP/UDP source、destination、direction
- UDP datagram長
- arrival timeとinter-arrival time
- RNVP v2 Outer Header
- 公開rate tierとtier遷移
- 過去に攻撃者自身が行ったdrop/delay操作
- 後から観測点を通過する逆方向cellのsizeとtiming

### 4.3 観測不可能情報

- AEAD plaintext
- Cell Role
- frameId、chunkIndex、codec flag、deadline
- DORA scoreとallocation
- 将来のcell
- 将来の自然損失trace
- receiverの復号結果。ただし外形として後から現れる応答は観測可能とする。

### 4.4 操作能力

中心実験はdrop-onlyとする。

```text
for every rolling W-cell window:
  attackDrops <= floor(rho * W)
  consecutiveAttackDrops <= b_max
```

| Parameter | Primary | Sensitivity |
|---|---:|---:|
| `W` | 200 cells | 100, 400 |
| `rho` | 0.02, 0.05 | 0.01, 0.03 |
| `b_max` | 3 cells | 1, 2, 5 |

攻撃判定は必ずcell到着時点までの履歴だけで行う。未来参照が検出されたrunは無効とする。

### 4.5 自然損失との合成

主実験では次の順序を`FIXED`とする。

```text
Sender -> Attack -> Natural Loss / Delay -> Receiver
```

攻撃損失と自然損失を別counterで記録し、重複dropは総損失へ二重計上しない。

## 5. RNVP v2 固定Cell仕様

### 5.1 Datagram長

- UDP payload長は`1200 bytes`を主条件とする。
- 感度分析では`800 bytes`、`1000 bytes`を使用できる。
- 1 run内でcell sizeを変更してはならない。
- IP/UDP headerを含むwire byteは別途計測する。
- IPv4 fragmentationまたはIPv6 fragmentationが発生したrunは主結果から除外し、失敗として記録する。

### 5.2 Cell layout

1200-byte主条件のlayoutを次で`FIXED`とする。

```text
+-------------------------------+ 0
| Outer Header        32 bytes  |  authenticated, not encrypted
+-------------------------------+ 32
| Encrypted Plaintext 1152 bytes|
|   Control Header     24 bytes |
|   Cell Body        1128 bytes |
+-------------------------------+ 1184
| AEAD Tag             16 bytes |
+-------------------------------+ 1200
```

任意の設定cell size `C` に対し、各領域長は次で計算する。

```text
outerHeaderBytes = 32
aeadTagBytes     = 16
plaintextBytes   = C - 48
controlBytes     = 24
cellBodyBytes    = C - 72
mediaPayloadMax  = C - 120
```

800、1000、1200以外のcell sizeはv0.1では拒否する。

### 5.3 Outer Header

すべてbig-endianでencodeする。C++ structを直接送信してはならない。

| Offset | Size | Field | Requirement |
|---:|---:|---|---|
| 0 | 4 | `magic` | `0x524E5632` (`RNV2`) |
| 4 | 1 | `version` | `2` |
| 5 | 1 | `outerFlags` | v0.1では0 |
| 6 | 2 | `outerHeaderBytes` | 32 |
| 8 | 8 | `connectionId` | CSPRNGでrunごとに生成 |
| 16 | 8 | `packetNumber` | 方向ごとに単調増加、再利用禁止 |
| 24 | 4 | `epochId` | sender-local Epoch番号 |
| 28 | 1 | `keyPhase` | v0.1では0 |
| 29 | 1 | `direction` | 0=forward、1=reverse |
| 30 | 2 | `reserved` | 0 |

Outer Header全体をAEADのAdditional Authenticated Dataとする。Outer HeaderにCell Role、payload length、frame情報、FEC情報を置いてはならない。

### 5.4 Encrypted Control Header

| Offset | Size | Field | Requirement |
|---:|---:|---|---|
| 0 | 1 | `innerVersion` | 1 |
| 1 | 1 | `cellRole` | Data/Fec/Repair/Dummy/Feedback/Control |
| 2 | 2 | `innerFlags` | role-specific |
| 4 | 4 | `streamId` | encrypted |
| 8 | 4 | `generationId` | FEC generation、未使用時0 |
| 12 | 2 | `generationIndex` | source indexまたはparity index |
| 14 | 2 | `generationSize` | source symbol数、2..8 |
| 16 | 4 | `feedbackSequence` | feedback以外は0 |
| 20 | 4 | `reserved` | 0 |

### 5.5 Cell Role

| Value | Role | Body |
|---:|---|---|
| 0 | Data | `MediaSymbolHeader + media payload + padding` |
| 1 | Fec | generation内Cell BodyのXOR parity |
| 2 | Repair | 元symbolと同一のMediaSymbol Body |
| 3 | Dummy | CSPRNG padding |
| 4 | Feedback | `FeedbackBody + padding` |
| 5 | Control | `ControlBody + padding` |

### 5.6 MediaSymbolHeader

DataまたはRepairのCell Body先頭48 bytesを次とする。

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `frameId` |
| 4 | 4 | `symbolId` |
| 8 | 8 | `captureTimeUs` |
| 16 | 8 | `deadlineTimeUs` |
| 24 | 2 | `chunkIndex` |
| 26 | 2 | `chunkCount` |
| 28 | 2 | `payloadLength` |
| 30 | 2 | `mediaFlags` |
| 32 | 4 | `codecConfigGeneration` |
| 36 | 4 | `sourceSequence` |
| 40 | 1 | `codecType` |
| 41 | 1 | `deadlineClass` |
| 42 | 2 | `reserved0` |
| 44 | 4 | `reserved1` |

1200-byte cellのmedia payload最大長は次である。

```text
1200 - Outer(32) - Tag(16) - Control(24) - MediaHeader(48)
= 1080 bytes
```

`payloadLength > 1080`、`chunkIndex >= chunkCount`、未知codec、期限形式不正のsymbolは破棄する。

### 5.7 FEC generation

- XORは暗号化前の固定長`Cell Body`全体に対して行う。1200-byte cellでは1128 bytes、1000-byte cellでは928 bytes、800-byte cellでは728 bytesである。
- v0.1のFEC sourceはData Bodyだけとし、Repair、Feedback、Control、Dummyをgenerationへ含めない。
- Data BodyにはMediaSymbolHeaderも含まれるため、1 symbol消失時にmetadataも復元できる。
- generationは2～8個のsource bodyと1個のparity bodyで構成する。
- 各source cellは同じ`generationId`、異なる`generationIndex`、同じ`generationSize`を持つ。
- sourceの`generationIndex`は0から`generationSize - 1`、FEC cellの`generationIndex`は`0xFFFF`とする。
- `generationId`はdirectionごとに単調増加し、同一session内で再利用しない。
- receiverは全indexのうち1個だけ欠け、parityが認証済みの場合にのみ復元する。
- 2個以上欠けたgenerationはXOR FECで復元しない。
- 復元bodyは、認証済みsource bodyと認証済みparityのXORから生成された場合だけ受理する。
- generationは最小member deadlineより前にparityが到達可能な場合だけ作る。

### 5.8 Repair

- Repairは元Data Bodyを新しいcellとして再送する。
- 新しい`packetNumber`とAEAD nonceを必ず使用する。
- 元ciphertextをそのまま再送してはならない。
- v0.1ではRepair cellを新しいFEC generationへ含めない。
- Repair発生によりEnvelopeのslot数を増やしてはならない。
- Repairが選択された分だけFECまたはDummy slotを減らす。

## 6. Cryptographic Processing

### 6.1 Algorithm

- AEADはAES-256-GCMを`FIXED`とする。
- Windows実装はCNG `BCrypt`を使用する。
- key derivationはHKDF-SHA-256を使用する。
- PSKは32 bytesとし、`RNVP_V2_PSK_HEX`から64桁hexで読み込む。
- v2有効時にPSKが欠落または不正なら起動を失敗させる。既定鍵へfallbackしてはならない。

### 6.2 Directional key derivation

```text
salt = big_endian(connectionId)
PRK  = HKDF-Extract(salt, PSK)

forwardKey = HKDF-Expand(PRK, "rnvp-v2/forward/key-phase-0", 32)
reverseKey = HKDF-Expand(PRK, "rnvp-v2/reverse/key-phase-0", 32)
forwardNoncePrefix = HKDF-Expand(PRK, "rnvp-v2/forward/nonce", 4)
reverseNoncePrefix = HKDF-Expand(PRK, "rnvp-v2/reverse/nonce", 4)
```

### 6.3 Nonce

```text
nonce[0..3]  = directionalNoncePrefix
nonce[4..11] = big_endian(packetNumber)
```

- packet numberは方向ごとに1から開始する。
- 同一`connectionId`、direction、keyPhaseでpacket numberを再利用してはならない。
- process内で使用済みconnectionIdを再利用してはならない。
- v0.1ではin-band rekeyを実装せず、30分または`2^32` cellの早い方でsessionを終了する。
- session継続が必要な場合は新しいconnectionIdで再確立する。

### 6.4 Replay protection

- receiverはdirectionごとに4096-bit sliding replay windowを持つ。
- windowより古いpacket、既受信packet、packet number 0を拒否する。
- AEAD verification成功前にreplay windowを更新してはならない。
- 認証失敗packetからconnection state、feedback state、rate stateを変更してはならない。

### 6.5 Security invariants

次は0件でなければならない。

- nonce再利用
- replay受理
- tag改変cell受理
- Outer Header改変cell受理
- 未認証feedbackによるsender state変更
- 未認証Controlによるapplication state変更
- 不正lengthによるbuffer overrun

## 7. Oblivious Real-Time Envelope

### 7.1 Epoch

- `epochDurationMs = 20`を主条件とする。
- 感度分析は10、30、40msとする。
- 各Epochのslot数`N_e`は公開rate tierだけから決める。
- 同一tier内では映像内容、IDR、FEC量、Repair量にかかわらずslot数を固定する。

### 7.2 Rate tier

1200-byte UDP payload、20ms Epochの既定tierを次とする。

| Tier | Cells/Epoch | UDP payload rate | Encoder data ceiling目安 |
|---:|---:|---:|---:|
| 0 | 4 | 1.92 Mbps | 1.34 Mbps |
| 1 | 6 | 2.88 Mbps | 2.01 Mbps |
| 2 | 9 | 4.32 Mbps | 3.02 Mbps |
| 3 | 13 | 6.24 Mbps | 4.36 Mbps |
| 4 | 18 | 8.64 Mbps | 6.04 Mbps |
| 5 | 25 | 12.00 Mbps | 8.40 Mbps |

- 初期tierは4とする。
- Encoder data ceilingはEnvelope payload rateの70%を上限目安とする。
- 30%はFEC、Repair、Control余地であり、未使用分はDummyにする。
- 実際の比較はIP/UDP header、AEAD、padding、feedbackを含むwire byteで行う。

### 7.3 Slot timing

Epoch内slot `i` の送信目標時刻を次とする。

```text
slotTime(e, i) = epochStart(e) + (i + 0.5) * epochDuration / N_e
```

- PacketPacerはslotTimeより早く送信してはならない。
- 送信時刻誤差をcellごとに記録する。
- deadlineを超えたData/Repairは送信せず、そのslotをDummyへ置換する。

### 7.4 Secret-independent tier control

rate tierが参照できる入力を次に限定する。

- aggregate delivery rate
- RTT
- aggregate authenticated feedback loss
- 公開pacing queue delay
- feedback timeout
- ECN。実装されている場合のみ。

次をtier decisionへ入力してはならない。

- keyframe、codec configuration、frame type
- DORA importance
- private per-frame backlog
- Repair対象の重要度
- decoder同期状態

### 7.5 Tier transition

- loss 2%以上、queue delay 40ms超、RTT増加50ms超のいずれかを2 sample連続で観測した場合、次Epochで1 tier低下する。
- feedbackを3 interval連続で失った場合、1 tier低下する。
- feedbackを5 interval連続で失った場合、Tier 0へ低下する。
- 2秒間安定した場合のみ1 tier上昇できる。
- 上昇は2秒に1回以下、低下は次の制御機会で実行する。
- Tier 0でも継続的な混雑信号がある場合は送信停止circuit breakerを作動させる。
- thresholdはpilotで感度確認できるが、test前に`PILOT-LOCK`する。

### 7.6 Queueとoverload

- MediaSymbol queueはdeadline昇順を基本とする。
- 収容不能時は、期限切れ、discardable、低importanceの順に送信前dropする。
- private drop decisionによってslot数を減らしてはならない。空いたslotはDummyにする。
- queue滞留上限は120msを初期値とする。
- `dataOverflow`、`deadlineDrop`、`dummyCell`、`repairStarved`を記録する。

## 8. Oblivious Feedback

### 8.1 Schedule

- reverse方向に40msごとに1 cellを送信する。
- lossがない場合も同じscheduleでcover feedbackを送る。
- feedback eventの有無によってcell size、個数、時刻を変更してはならない。
- 送る情報がない場合はvalidな空bitmapを暗号化する。

### 8.2 Feedback Body

Cell Body先頭72 bytesを次とし、残りをpaddingする。

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | `basePacketNumber` |
| 8 | 32 | `receivedBitmap[256]` |
| 40 | 8 | `latestReceiveTimeUs` |
| 48 | 8 | `echoPacketNumber` |
| 56 | 8 | `echoSendTimeUs` |
| 64 | 4 | `feedbackFlags` |
| 68 | 4 | `reserved` |

feedbackは累積またはoverlapするbitmapを使用し、単一feedback lossで256-cell区間が永久に失われないようにする。

## 9. DORA-v0

### 9.1 責務

DORA-v0は各Epochで、既に決まった`N_e` slotをData、FEC、Repair、Dummyへ割り当てる。DORAは`N_e`またはtierを変更してはならない。

### 9.2 Symbol state

```text
SymbolState {
  symbolId
  frameId
  codecRole
  mediaFlags
  descendantsWithinHorizon
  remainingDeadlineUs
  transmittedCopies
  acknowledged
  recovered
  feasibleRepairRounds
  payloadBytes
}
```

### 9.3 Importance score

v0の初期scoreを次とする。係数はtraining setだけで調整し、testでは固定する。

```text
q = 1.0
  + 7.0 * isDecoderSync
  + 7.0 * containsCodecConfig
  + 5.0 * isIdr
  + 2.0 * isNonDiscardable
  + 0.25 * min(descendantsWithinHorizon, 8)

urgency = 1.0 + clamp((100ms - remainingDeadline) / 100ms, 0, 1)
q_final = q * urgency
```

- H.264 dependency情報が取得できない場合、`descendantsWithinHorizon = 0`とし、推測値を入れてはならない。
- Oracle damage labelをonline scoreへ入力してはならない。
- score係数、変更理由、training commitをmanifestへ保存する。

### 9.4 Feasible action

1 Epochのactionは次を満たす。

```text
dataCells + fecCells + repairCells + dummyCells = N_e
all selected media cells can be sent before deadline
FEC generation size in {2, 4, 8}
repairCells <= configuredRepairCap
```

### 9.5 Candidate generation

DORA-v0は次のbounded candidateだけを列挙する。

1. Uniform g4 FEC
2. Uniform g2 FEC
3. Uniform g8 FEC
4. 上位importance symbolをg2、その他をg4
5. 上位importance symbolをg2、その他をg8
6. 0～`repairCap`個のdeadline-feasible Repairと上記FECの組合せ
7. FECなし、Repairのみ。低slot tierのablation用。

候補数上限は256 action/Epochとする。

### 9.6 Loss scenario

training時に固定した次のscenario集合でcandidateを評価する。

- independent loss: 1%、2%、5%
- burst loss: length 2、3
- rolling-window selective loss: `rho=0.02`、`0.05`
- feedback unavailable: 1、3 interval

### 9.7 Proxy loss

```text
proxyLoss(action, scenario) =
    sum_j q_final(j) * P(j unrecoverable before deadline)
  + lambdaLate * predictedLateCells
  + lambdaOverflow * predictedOverflowCells
  + lambdaFallback * requiresFallback

score(action) = max_scenario proxyLoss(action, scenario)
```

Dummy数そのものへ罰則を与えてはならない。Dummyは固定Envelopeを守るための残余であり、Dummy罰則がprivate backlogから外部scheduleを変える誘因になるためである。Dummy比率は効率指標として別に報告する。

### 9.8 実行時間とfallback

- decision budgetは5msとする。
- 5ms以内に完了しない、候補が不正、例外が発生した場合はUniform g4へfallbackする。
- `p50`、`p95`、最大decision time、fallback率を記録する。
- `TARGET`: p95 2ms以下、最大5ms以下、fallback率1%未満。

## 10. Sender State Machine

```text
Disabled
  -> KeyReady
  -> Running
  -> Draining
  -> Closed

Any state -> Fault
```

### 10.1 State requirement

| State | Behavior |
|---|---|
| Disabled | v1動作または送信停止 |
| KeyReady | PSK検証、connectionId生成、key derivation |
| Running | Epoch生成、DORA、AEAD、送信 |
| Draining | 新規Data受付停止、期限内queueだけ処理 |
| Closed | key materialをzeroize、socket終了 |
| Fault | media送信停止、原因を記録、安全channelは維持 |

### 10.2 Epoch処理

```text
onEpochStart:
  publicState = readCongestionState()
  tier = updateTier(publicState)
  N = cellsPerEpoch(tier)
  symbols = collectDeadlineFeasibleSymbols()
  action = dora.allocate(symbols, N)
  cells = materializeDataFecRepairDummy(action, N)
  assert cells.size == N
  encryptAndSchedule(cells)
```

## 11. Receiver State Machine

```text
Idle
  -> CandidateConnection
  -> Authenticated
  -> Active
  -> Closed

CandidateConnection -> Rejected
Any state -> Fault
```

- 未知connectionIdの最初のcellはAEAD成功後にだけsessionを生成する。
- 同時CandidateConnectionは8件までとする。
- AEAD失敗が1秒に100件を超えたsourceは1秒間rate-limitする。
- Active stateではReplay Check、role dispatch、FEC recovery、frame reassemblyの順で処理する。
- Dummyは認証後に破棄する。

## 12. Selective Attack Simulator

### 12.1 Interface

```text
AttackDecision Decide(const AttackObservation& observation,
                      const AttackHistory& causalHistory,
                      const AttackBudgetState& budget);
```

`AttackObservation`へtruth labelを含めてはならない。

### 12.2 共通観測feature

- direction
- cell size
- packet number gap
- inter-arrival time
- Epoch内slot index
- public tier
- 過去32/128 cellのtiming統計
- 過去の自分のdrop pattern
- 逆方向cellのtiming

RNVP v1攻撃時だけ、wire上で平文のpacketType、flags、frameId、chunkIndex、codecTypeを追加featureとして使用できる。

### 12.3 攻撃family

| ID | Attacker | Requirement |
|---|---|---|
| A0 | No Attack | 自然損失のみ |
| A1 | Blind Random | trace非依存乱数 |
| A2 | Blind Periodic | 固定周期 |
| A3 | Blind Burst | 予算内固定burst |
| A4 | Causal Heuristic | timing、size、feedback shapeを使用 |
| A5 | Causal Learned | training setで学習、onlineは因果featureのみ |
| A6 | Header Oracle | 真のrole/headerを使用 |
| A7 | Marginal-Damage Oracle | offline counterfactual labelを使用 |
| A8 | Combinatorial Oracle | rolling window内beam search |

### 12.4 Budget enforcement

- Simulator本体がattackerの外側でbudgetを強制する。
- 予算超過drop要求は`DeniedByBudget`として記録し、packetを通過させる。
- defenseごとに攻撃器を再学習する。
- training session数、feature数、update回数、random restart、CPU/GPU時間を記録する。

## 13. 計測仕様

### 13.1 共通ID

以下を全logで使用する。

```text
runId, defenseId, attackId, videoId, networkTraceId,
attackSeed, connectionId, streamId
```

### 13.2 Cell event log

`logs/blinddrop_cell_<runId>.csv`

必須列:

```text
timestampUs,direction,epochId,slotIndex,packetNumber,udpBytes,wireBytes,
publicTier,sendTargetUs,sendActualUs,attackRequested,attackApplied,
naturalDrop,received,aeadValid,replayRejected,
truthRole,truthFrameId,truthSymbolId,truthImportance,
generationId,generationIndex,generationSize,deadlineUs,
doraDecisionId
```

`truth*`列はlogger専用であり、attacker input生成処理と別objectに保持する。

### 13.3 Frame/display event log

`logs/blinddrop_frame_<runId>.csv`

```text
frameId,captureUs,encodeEndUs,firstSendUs,lastSendUs,
firstReceiveUs,reassemblyCompleteUs,decodeEndUs,displayUs,
deadlineUs,decoded,displayed,fecRecovered,repairRecovered,
missingSymbols,freezeStartUs,freezeDurationUs,
isIdr,isDecoderSync,containsCodecConfig
```

### 13.4 Epoch/DORA log

`logs/blinddrop_epoch_<runId>.csv`

```text
epochId,tier,N,dataCells,fecCells,repairCells,dummyCells,
queuedSymbols,droppedBeforeSend,dataOverflow,
doraCandidateCount,doraDecisionUs,doraFallback,
predictedProxyLoss,actualDeadlineMisses
```

### 13.5 Task event log

`logs/blinddrop_task_<runId>.csv`

```text
taskTimeUs,taskId,eventType,positionX,positionY,
commandId,commandSendUs,visualConfirmationUs,
safetyStop,collision,pathDeviation,taskComplete
```

### 13.6 Manifest

`logs/blinddrop_manifest_<runId>.json`

必須項目:

- git commit、dirty worktree flag
- executable hash
- protocol versionとcell size
- 全環境変数とconfig
- PSKそのものを除くkey identifier hash
- dataset split ID
- defense/attack parameter
- random seed
- video hash、network trace hash
- OS、CPU、GPU、build configuration
- run開始・終了時刻
- invalid run理由

## 14. 被害指標

### 14.1 Freeze Time Ratio

```text
theta = max(100ms, 3 * T_frame)

freezeDuration_i =
  max(0, displayGap_i - T_frame), if displayGap_i > theta
  0, otherwise

D = sum(freezeDuration_i) / measuredSessionDuration
```

50、100、150msのthreshold感度分析も報告する。

### 14.2 Damage Advantage

```text
DA_raw = D_selective - D_blind

NDA = (D_selective - D_blind)
    / (D_oracle - D_blind + epsilon)
```

`epsilon = 1e-6`とする。`D_oracle - D_blind < 0.005`の場合、NDAを主解釈に使用しない。

### 14.3 必須副指標

- 最大連続freeze
- 500ms以上freeze発生率
- decoder同期回復時間
- undecodable frame率
- deadline内goodput
- p50/p95/p99 capture-to-display delay
- display Age of Information
- wire bytesとgoodput/wire ratio
- Dummy比率
- DORA decision timeとfallback率
- AEAD CPU時間
- pacing schedule error
- task成功率、完了時間、安全停止、衝突

## 15. 実験設計

### 15.1 Dataset split

映像およびnetwork traceを次で完全分離する。

| Split | Minimum | Purpose |
|---|---:|---|
| Pilot | 4 videos × 4 traces × 5 seeds | 分散、実行時間、guardrail確認 |
| Training | 8 videos × 8 traces × 5 seeds | DORA/attacker/baseline調整 |
| Test | 10 videos × 10 traces × 10 seeds | 最終確認分析 |

- 同一映像の別区間を複数splitへ入れてはならない。
- 同一network traceを変形して複数splitへ入れてはならない。
- 最終test開始後はparameterを変更してはならない。

### 15.2 Primary media condition

pilot開始時の主条件を次とする。pilot結果に基づく変更は可能だが、test前に`PILOT-LOCK`する。

| Item | Initial value |
|---|---|
| Codec | H.264 |
| Resolution | 1280×720 |
| FPS | 30 |
| Encoder target | 6 Mbps |
| GOP/IDR | 2秒 |
| Cell | 1200-byte UDP payload |
| Epoch | 20ms |
| Initial tier | 4、8.64 Mbps payload envelope |
| Session | 60秒、先頭5秒warm-up除外 |
| Attack start | 10秒 |
| Natural RTT | 40ms |
| Natural loss | 0.5% |
| Attack budget | 2%、W=200、burst max=3 |

### 15.3 Baseline

| ID | Defense |
|---|---|
| B0 | RNVP v1 |
| B1 | RNVP v1 + Inner Header AEAD |
| B2 | Fixed Cell + Envelope + Uniform g4 |
| B3 | Fixed Envelope + Public UEP |
| B4 | Fixed Envelope + Deadline-aware FEC/Repair |
| B5 | Fixed Envelope + DORA、oblivious feedbackなし |
| B6 | BlindDrop-RT full |

### 15.4 Fairness

- 同じ事前encode済み映像を使う。
- 同じ有効Data bytesを使う。
- 同じaverage wire-byte ceilingを使う。
- 同じlatency guardrailを使う。
- 同じvideo/trace/seed組をpairedで使う。
- parameter探索回数と範囲を揃える。
- defenseごとにattackerを再学習する。
- baselineがbudgetを超えたrunを黙って除外せず、constraint failureとして報告する。

### 15.5 統計

- 実験単位は`video × networkTrace × attackSeed`とする。
- packetまたはframeを独立sampleとして扱わない。
- cluster-aware paired bootstrapを10,000 resample行う。
- mean、median、p95、95% CIを報告する。
- `delta_DA`と非劣性marginはpilot後、test前に`PILOT-LOCK`する。
- H0～H5の確認分析には多重比較補正を行う。
- 探索分析を確認分析として報告してはならない。

## 16. Gateと合否条件

### Gate 0: 測定基盤

次をすべて満たす。

- 同一seedのrunが同一attack/natural-loss decisionを再現する。
- cell、frame、display logを共通IDでjoinできる。
- replay集計とlive reporterの主要指標差が許容誤差内である。
- invalid runを自動検出できる。

### Gate 1: 攻撃成立性

RNVP v1の事前登録主条件で次を満たす。

```text
lower95CI(DA_raw) > delta_DA
```

満たさない場合、Defense実装を研究結論の中心に進めず、攻撃が成立しない条件を整理する。

### Gate 2: Cell/Envelope実行可能性

機能要件:

- 全送信UDP payloadが設定cell sizeと一致する。
- 同じpublic network historyとtier historyでは、異なる映像でもEpoch slot数が一致する。
- nonce、replay、AEAD invariant violationが0件である。
- feedbackが40ms周期で送信される。
- overload時もslot数を減らさずDummyへ置換する。

`TARGET`性能要件:

- g4 baseline比平均追加wire byte 15%以下
- p95追加capture-to-display delay 40ms以下
- slot send error p95 2ms以下
- DORAなしのCPU増加15%以下

### Gate 3: DORA追加価値

同一wire・latency予算で、DORAがUniform g4に対し次を満たす。

- `D_selective`または`DA_raw`の一方で事前登録した優越性
- もう一方で事前登録した非劣性
- 自然損失時QoEで非劣性
- DORA fallback率1%未満

両主指標で優越した場合をstrong successとする。

### Gate 4: CPS有効性

- 映像改善がAge of Information、操作中断、task成功率の少なくとも一つで最小実用差以上残る。
- 衝突、安全停止、自然損失時task性能を非劣性margin以上悪化させない。
- Gate 4不成立時は主張をreal-time video transportへ限定する。

## 17. Functional Test Specification

| Test ID | Test | Expected |
|---|---|---|
| T-PROTO-01 | v2 cell encode/decode round trip | field完全一致 |
| T-PROTO-02 | 800/1000/1200 cell size | 常に指定長 |
| T-PROTO-03 | reserved/length不正 | reject |
| T-CRYPTO-01 | 1 bit ciphertext改変 | reject、state不変 |
| T-CRYPTO-02 | Outer Header改変 | reject、state不変 |
| T-CRYPTO-03 | 1,000万nonce生成 | duplicate 0 |
| T-CRYPTO-04 | packet replay | 受理0 |
| T-CRYPTO-05 | replay window境界 | 仕様通りaccept/reject |
| T-FEC-01 | generation内1 loss | body完全復元 |
| T-FEC-02 | generation内2 loss | 復元しない |
| T-FEC-03 | parity改変 | AEAD reject、復元しない |
| T-ENV-01 | 静止/高motion映像、同一public state | slot schedule一致 |
| T-ENV-02 | IDR発生 | slot数不変 |
| T-ENV-03 | Repair発生 | slot数不変 |
| T-ENV-04 | queue overflow | deadline drop + Dummy置換 |
| T-FB-01 | lossなし | cover feedback継続 |
| T-FB-02 | feedback loss | 次回bitmapでoverlap回復 |
| T-DORA-01 | decision timeout | Uniform g4 fallback |
| T-DORA-02 | N cells割当 | role合計が常にN |
| T-ATTACK-01 | causal audit | future access 0 |
| T-ATTACK-02 | rolling budget | 全windowで違反0 |
| T-LOG-01 | attacker input audit | truth field混入0 |
| T-REPLAY-01 | run replay | attack decision再現 |

## 18. Code Mapping

### 18.1 新規file

| File | 内容 |
|---|---|
| `network/BlindDropProtocol.h/.cpp` | v2 field、encode/decode、validation |
| `network/BlindDropCrypto.h/.cpp` | HKDF、AES-GCM、nonce、key zeroization |
| `network/BlindDropReplayWindow.h/.cpp` | 4096-bit replay window |
| `network/BlindDropEnvelope.h/.cpp` | Epoch、tier、slot、Dummy |
| `network/DoraAllocator.h/.cpp` | state、candidate、score、fallback |
| `network/ObliviousFeedback.h/.cpp` | bitmap、40ms schedule |
| `network/SelectiveAttackSimulator.h/.cpp` | attacker interfaceとbudget enforcement |
| `network/BlindDropMetrics.h/.cpp` | cell/frame/epoch/task event |
| `network/BlindDropReporter.h/.cpp` | DA、NDA、Gate、Pareto |
| `network/CpsExperimentAdapter.h/.cpp` | control/task event bridge |

### 18.2 変更file

| File | 変更 |
|---|---|
| `network/NetworkManager.*` | v1/v2 dispatch、sender integration |
| `network/UdpReceiver.*` | v2 detect、AEAD、replay、role dispatch |
| `network/PacketPacer.*` | slot-time scheduling mode |
| `network/FrameReassembler.*` | recovered MediaSymbol入力 |
| `network/NetworkConditionSimulator.*` | Attack→Natural Lossの二段処理 |
| `network/NetworkExperimentRunner.*` | defense/attack matrix、seed固定 |
| `network/NetworkExperimentReporter.*` | BlindDrop Gate集計 |
| `network/NetworkExperimentReplay.*` | event replay |
| `network/NetworkCsvLogger.*` | 新event schema |
| `application/AppMain.cpp` | feature flag、component wiring、telemetry |
| `application/AppImGuiLayer.cpp` | Live Arena表示。研究機能完成後のみ |
| `GE3.vcxproj` / `.filters` | 新file登録 |

### 18.3 実装分離

- 暗号処理を`PacketProtocol.h`へ直接追加しない。
- DORAを`AppMain.cpp`へ実装しない。
- attack truthとattacker observationを同じstructで表現しない。
- Loggerのためにtransport decisionを変更しない。
- UIからしか変更できないparameterを作らない。すべてmanifest化可能なconfigを持たせる。

## 19. Configuration

### 19.1 Protocol / Crypto

| Environment variable | Default |
|---|---|
| `RNVP_PROTOCOL_VERSION` | `1` |
| `RNVP_V2_PSK_HEX` | なし。v2時必須 |
| `RNVP_V2_CELL_BYTES` | `1200` |
| `RNVP_V2_REPLAY_WINDOW` | `4096` |
| `RNVP_V2_MAX_SESSION_SEC` | `1800` |

### 19.2 Envelope

| Environment variable | Default |
|---|---|
| `BLINDDROP_EPOCH_MS` | `20` |
| `BLINDDROP_INITIAL_TIER` | `4` |
| `BLINDDROP_FEEDBACK_MS` | `40` |
| `BLINDDROP_QUEUE_MAX_MS` | `120` |
| `BLINDDROP_DATA_CEILING_RATIO` | `0.70` |
| `BLINDDROP_DORA_BUDGET_MS` | `5` |

### 19.3 Attack

| Environment variable | Default |
|---|---|
| `BLINDDROP_ATTACK_TYPE` | `none` |
| `BLINDDROP_ATTACK_RHO` | `0.02` |
| `BLINDDROP_ATTACK_WINDOW` | `200` |
| `BLINDDROP_ATTACK_BURST_MAX` | `3` |
| `BLINDDROP_ATTACK_SEED` | 必須。自動値禁止 |

### 19.4 Validation

- 未知値、範囲外値、parse失敗は起動時errorとする。
- confirmation testではdefault substitutionを禁止する。
- 有効値と値のsourceをmanifestへ保存する。
- secretはmanifestへ平文保存しない。

## 20. 実装順序

### Phase 0: Measurement and Attack Benchmark

1. 共通run IDとmanifest
2. cell/frame/display event log
3. blind/random/burst attacker
4. RNVP v1 causal/header attacker
5. Oracle attacker
6. DA/NDA reporter
7. Gate 1判定

Gate 1不成立時は、成立条件の探索とnegative result整理を優先する。

### Phase 1: RNVP v2 Security Core

1. BlindDropProtocol
2. HKDF/AES-GCM
3. ReplayWindow
4. Data/Dummy/Feedback round trip
5. security invariant test

### Phase 2: Envelope

1. Epoch/slot scheduler
2. Dummy置換
3. 固定feedback
4. tier controller
5. overload/circuit breaker
6. Gate 2判定

### Phase 3: Recovery

1. fixed-body XOR generation
2. Repair cell
3. Uniform g2/g4/g8 baseline
4. Public UEP baseline
5. deadline-aware baseline

### Phase 4: DORA-v0

1. SymbolStateとimportance
2. bounded candidate generation
3. proxy loss
4. timeout/fallback
5. ablationとGate 3

### Phase 5: CPS and Final Evaluation

1. CpsExperimentAdapter
2. task scenario固定
3. paired test matrix
4. Gate 4
5. BlindDrop Live Arena
6. artifact manifestと再現手順

## 21. Research Deliverables

1. RNVP v1に対する観測適応型攻撃benchmark
2. RNVP v2 fixed-cell secure transport
3. Oblivious Real-Time Envelope
4. DORA-v0と強いbaseline実装
5. reproducible video/network/attack split
6. cell-to-task event dataset
7. BlindDrop ReporterとGate判定
8. BlindDrop Live Arena
9. 成立条件、非成立条件、制約を含む研究報告

## 22. Definition of Done

研究実装を完了と呼ぶには、次をすべて満たす必要がある。

- T-PROTO、T-CRYPTO、T-FEC、T-ENV、T-FB、T-DORA、T-ATTACK、T-LOG、T-REPLAYが合格する。
- v1とv2の同一binary比較が再現できる。
- 全runがmanifestから再実行できる。
- test setがtraining code pathから隔離されている。
- 7方式の同一budget比較が完了している。
- Gate 1～3の結果がCI付きで出力される。
- CPSを主張する場合はGate 4を満たす。
- 未達成TARGETを達成済みとして記述していない。
- 攻撃予算、観測点、codec、cell size、自然損失の適用範囲を明記している。
- 第三者networkを攻撃せず、閉鎖環境またはsimulatorだけで実験している。

## 23. 未決事項

次は実装前またはpilotで決定し、decision logへ残す。

| ID | Decision | Deadline |
|---|---|---|
| O-01 | Windows CNG HKDFを直接使うか、HMAC-SHA256でHKDFを実装するか | Phase 1開始前 |
| O-02 | H.264 dependency/descendant情報をどこまで抽出するか | Phase 4開始前 |
| O-03 | `delta_DA`と非劣性margin | Pilot終了時 |
| O-04 | primary network traceとtask scenario | Test凍結前 |
| O-05 | 人間操作者を使うか、固定control policyを主分析にするか | Gate 4開始前 |
| O-06 | 物理ロボットとsimulationの役割分担 | Gate 4開始前 |
| O-07 | 攻撃器の公開範囲 | artifact公開前 |

未決事項を暗黙のdefaultでtestへ持ち込んではならない。
