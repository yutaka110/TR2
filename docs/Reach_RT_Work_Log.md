# Reach-RT 作業ログ

現在の状態と次の作業：[Reach_RT_Implementation_Progress.md](Reach_RT_Implementation_Progress.md)  
研究仕様：[Reach_RT_Research_Spec.md](Reach_RT_Research_Spec.md)

このファイルは実際の作業履歴を追記する。過去の結果は、その時点のコードと環境に対する結果として保存する。訂正する場合も訂正日と理由を残す。

## E001 — 2026-09-21：実装手順と進捗管理の開始

### 対象

G0-01、G0-02、G0-03の開発環境確認部分。

### 行ったこと

1. 研究仕様v0.1.0と既存コードを照合した。
2. Reach-RT固有のモジュール名と研究モードを、主要ソース・文書・プロジェクト設定から検索した。
3. Loopback、H.264設定、期限付き回復、回線模擬、実験記録、表示基盤を確認した。
4. Visual Studio Installerの`vswhere`でMSBuildのインストール先を確認した。
5. G0～G6の38タスク、各完了条件、更新ルールを記載した進捗管理ファイルを作成した。
6. 仕様書に進捗管理と作業ログへの参照を追加した。

### 確認したソース状態

- HEAD：`467b475`（2026-06-13のmerge commit）。
- 本作業前から存在した追跡済み変更：`README.md`、`application/AppMain.cpp`、`network/FrameReassembler.cpp`、`network/NetworkCsvLogger.cpp`、`network/NetworkManager.cpp`、`network/NetworkManager.h`、`network/NetworkStats.h`、`network/UdpReceiver.cpp`、`network/UdpReceiver.h`。
- 本作業前から存在した未追跡文書：BlindDrop関連3文書と`docs/Reach_RT_Research_Spec.md`。
- 未コミット変更を含むため、HEADだけでは現行実装を再現できない。基準性能の記録時に差分・実行ファイル・設定のハッシュも保存する。

### 開発環境の確認結果

| 項目 | 結果 |
|---|---|
| Visual Studioの所在 | `C:\Program Files\Microsoft Visual Studio\18\Community` |
| MSBuildの所在 | `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe` |
| プロジェクトの要求 | v145 / Windows SDK 10.0.26100.0 / x64 / C++20 |
| 既定の出力先 | `$(SolutionDir)..\generated\outputs\$(Configuration)\` |
| `msbuild` / `cl`のPATH検索 | 今回のPowerShellでは解決されなかった。コンパイラ未導入を意味しない |
| 必要コンポーネント全体 | 未確認 |
| 実ビルド | 未実施 |
| アプリ実行・性能測定 | 未実施 |

### 得られた結論

- 映像通信エンジンの基盤はコード上に存在する。
- Reach-RT専用の仮想作業、受信映像による閉ループ、容量付き双方向回線、専用スケジューラの実装は、今回の確認範囲で見つかっていない。
- 研究仕様の作成済み状態と、研究システムの実装・性能検証済み状態を分けて管理する必要がある。
- 現在地をG0途中、研究ゲート通過0 / 7とした。G0-01とG0-02は完了、G0-03は作業中。

### 成果物と確認

- 作成：`docs/Reach_RT_Implementation_Progress.md`。
- 作成：`docs/Reach_RT_Work_Log.md`。
- 更新：`docs/Reach_RT_Research_Spec.md`の進捗管理への参照。
- 文書内の参照先、タスクIDの一意性、38件の状態集計、UTF-8、Markdownコードフェンスを確認。
- 本エントリで研究コードの実装完了や、実験合格を報告していない。

### 残りの作業・次の一手

G0-03として、出力先・依存関係・ビルド環境を確認し、既存版のビルド結果を保存する。続いてG0-04の基準動作・性能記録へ進む。

## E002 — 2026-09-21：G0のビルドと既存版基準試験

対象ID：G0-03、G0-04。着手時は環境の所在のみ確認済み、実ビルド・実行は未実施。

### 実装意図と変更

- 元の未コミット変更9ファイルを維持し、既存エンジンが実行可能かを先に確認した。
- `tools/build_reach_g0.ps1`と`reach_g0_paths.props`で、ビルド出力を作業領域内に分離した。
- `tools/run_reach_g0_baseline.py`で、試行ごとのResources/config、ログ、差分、ソース/実行ファイルSHA-256を保存するようにした。実験条件の取り違えと過去ログの上書きを防ぐためである。
- `tools/summarize_reach_g0.py`で、事前指定の正常/10%損失×3 seedだけを集計した。

### 結果

- v145、SDK 10.0.26100.0、Release x64のビルド成功。基準版は警告28・エラー0。
- 初期失敗はMSBuild子プロセス環境のPath/PATH重複。Pythonによる環境名正規化で解消。並列数は1。マシン設定は変更しなかった。
- カメラなし、生成H.264、loopback、20秒、warmup 5秒、seed 101/202/303で正常と10%損失を逐次実行。6回とも終了コード0、シナリオ一致、復号ログあり。
- 各試行平均表示fpsの平均は正常18.91、損失14.98。既存適応処理での基準値であり、Reach-RTの効果ではない。
- CPU i7-13620H、メモリ約15.71 GiB、Intel UHD/RTX 4060 Laptop GPU。符号化autoはNVIDIA MFT有効化失敗後Intel Quick Syncを選択。

### 証拠・制約

証拠は`artifacts/reach_g0_20260921/`の`build_Release_baseline*`、`machine.json`、`baseline_summary.json`、`runs/baseline_{101,202,303}`と`runs/loss10_{101,202,303}`。基準exeを`bin/Release/GE3_baseline.exe`として保存した。SHA-256はG0確認書3章に記載。

WMI/CIMはアクセス不可のためレジストリ/DXGI/native memory APIで代替。電源設定やOSスケジューリングは固定していない。計測窓はログ上約14.01秒。既存遅延のフレーム対応に問題が見つかったため、遅延の研究結果としては採用しない（E003）。試行数3から優位性を推定しない。

G0-03、G0-04を完了へ更新。基準試験時点の実行ファイルと、その後の診断追加版を別に記録する。

## E003 — 2026-09-21：実コーデック検査と時刻対応問題の発見

対象ID：G0-05。設定値だけから参照構造・低遅延性を仮定しないことを目的とした。

### 実装と確認

- `tools/reach_g0_codec_probe.cpp/.vcxproj`で既存H264Encoderに640×360 NV12、1.5Mbps、30fps、90枚を入力。入力番号45前にIDR要求し、出力AUと要求履歴を保存。
- `tools/inspect_reach_g0_codec.py`でAnnex-B/SPSの一部/slice種別を検査。参照グラフ全体は未検証と明示。
- `tools/run_reach_g0_codec.py`でビルド・auto/software実行・ハッシュ・構文解析を再実行可能にした。`codec_repro_check`に最終実行を保存。
- `network/NetworkVideoReceiver.h/.cpp`でMFTの出力PTSを取得し、受信CSVに入力PTS/出力PTS/有効性を追加。
- 診断版をビルドし、正常12秒の`pts_audit_auto`を実行。`tools/summarize_reach_g0_pts.py`で集計。

### 実測と判断

1. auto/softwareとも90 AU、I 4/P 86/B 0、SPS最大参照数1、検査範囲の構文エラー0。
2. IDR出力通番はauto 0/28/45/73、software 0/30/45/75。要求と実IDRを別イベントとして記録した。
3. softwareの最初の出力は入力呼出16、再確認時約539ms。低遅延設定/Bなしでも出力バッファリングがある。
4. 受信時刻監査は記録対象の復号成功180件中180件で出力PTSが入力より50ms古い。現在入力のID/時刻を返った画像へ付けるコード上の仮定が成立しないことを確認した。
5. 実codecは保守的IDR世代モデルを採用。単純IP参照は理論モデルの仮定に限定する。

### 失敗・未完了事項

初版のcodec probeはDrainOutputの待ちタイムアウトを失敗と数えた。既存APIの意味を追跡し、入力/flush失敗と「イベントなしまたはエラー」を分離した。修正前結果も保存した。構文解析器のSP/SIラベルも修正した。

時刻の対応付け自体は未修正。完全なPPS/MMCO/参照リスト解析も未実施。G0で不具合を確認したことを、修正完了としない。G1-03でencoder/decoderの出力PTSから元の撮影を照合する台帳と画像内IDによる検証、G2-04で欠損・再同期の検証を行う。

G0-05は「制約と採用モデル・観測方法の確定」として完了。G1-03の完了条件を具体化した。診断追加版の実行結果をE002の6試行へ混ぜていない。

## E004 — 2026-09-21：比較方式・初期タスク・G0判定の確定

対象ID：G0-06、G0-07。

### 行ったことと意図

- Hairpin本文と訂正/公式実装、Tooth本文、TAMS、Age of Loop、Viabilityの関連節を確認した。既知のアイデアを新規と主張せず、実装差を隠さないためである。
- B1の期限計画、B2の復号情報追加、B3の作業情報追加と共通候補・共通予算を具体化した。原論文再現ではない独自方式として表示する。RSとXORの能力差はG3で解消するか結論を限定する。
- T1/T2の座標・マーカー・姿勢推定・制御則・制動・ウォッチドッグ・真値境界をINITIALとして決定した。現時点で閉ループ動作済みとはしていない。
- `docs/Reach_RT_G0_Assessment.md`を作成。機能の目的、実装状態、実測の限界、対外説明例、再実行方法、G1への条件をまとめた。
- `docs/reach_rt_initial.json`を作成。現アプリ未対応の設計資料と明示した。
- 研究仕様をv0.1.1へ更新し、G0確認書への参照、保守的モデル、PTS照合の必須条件を追記した。

### 最終確認

Python 5ファイルの構文、PowerShellの構文、追加props/vcxprojのXML、初期設定JSON、18初期姿勢の組合せ、文書のUTF-8・コードフェンス・ローカル参照、38件の一意なタスクIDと完了7件を確認した。`git diff --check`はエラーなし（既存の改行変換通知あり）。機械可読な検査結果は`artifacts/reach_g0_20260921/final_checks.json`に保存。C++の変更は診断版Releaseビルドと`pts_audit_auto`、プローブは`codec_repro_check`で動作確認した。設計した制御器の動作試験はG1で行う。

### 現在地と次の作業

G0-06/07の設計作業を完了、G0全7項目を完了とした。研究ゲートは1/7。ビルド・基準動作・コーデック制約・初期タスク・比較方針が明確というG0条件を満たした。時刻誤対応を放置して研究測定へ進むことは認めない。

次はG1-01の研究設定・共通時計・ログとG1-02/03の世界・撮影対応。その後、受信画素による固定制御と逆方向UDPを接続し、理想条件の18初期姿勢で検証する。研究の改善率、成功率、2ms計算目標はいずれも未検証。

## E005 — 2026-09-21：G1-01研究モードの実装と検証

対象ID：G1-01。着手時は未実装。今回の区切りは研究モードの土台までとし、G1-02～06は未着手のまま管理する。

### 何を、なぜ実装したか

- 同じGE3.exeの`AppMain::Run`で研究モードを分岐する入口を追加した。既存実験と研究用の条件・結果を混同しないためである。
- `research/ReachJson.*`と`ReachFoundation.*`に、上限付きJSON読取り、厳密な設定検査、共通単調時計、周期期限、GUID、SHA-256、セッション記録を実装した。
- `ReachResearchMode.cpp`に、画面あり/なしの実行、周期超過判定、状態と時計通知数を表示する基盤画面を追加した。
- `config/reach_rt_g1_foundation.json`は、今動く機能だけの実行用設定とした。G0設計JSONの未実装値を読み込んだように見せない。
- `tools/run_reach_g1.py`、`open_reach_g1.cmd`を追加。実行ごとの条件・差分・新規研究ソース・実行結果を保存する。
- `test_reach_g1.cpp`、`reach_g1_tests.vcxproj`、`verify_reach_g1.py`、`verify_reach_g1_legacy.py`を追加し、設定誤り・時刻・同時記録・実際のモード分岐を検証可能にした。
- `docs/Reach_RT_G1_Foundation.md`に対外説明例、実装意図、構成、結果の意味、起動方法、未完了項目をまとめた。

### 検証した状態と結果

- HEADは`467b475`、以前からの未コミット変更を維持。今回は新規researchファイル、プロジェクト登録、AppMainの入口6行を追加した。H.264/復号対応のコードは今回変更していない。
- Release x64ビルド成功。新規コードの型変換警告修正後の増分ビルドは警告0/エラー0。クリーンビルド全体の警告0を意味しない。
- exe SHA-256：`446683a698684cd972248dd09800d03ab15f96f1570455a063ccf158acc7b495`。
- 単体検証：合格（663アサーション。大半は記録各行のID/連番/時刻検査）。30 Hzを1時間相当進めて108,000通知、4スレッド計200イベントの欠落なし。壁時計1時間の耐久試験ではない。
- 実アプリ統合12ケース：正常T1/T2ラベル、不正モード・設定8ケース、同時起動2ケースを確認。設定/exeハッシュ、時計通知の集計、連番、session分離を検証。
- 画面なし2秒実行：physics 200/camera 60/control 40通知、正常終了。
- 画面あり2秒実行：ウィンドウ作成経路を通り、同数の通知と`foundation_completed`を記録。完了後の画面は閉じるまで残す設計。
- 既存モード確認：同じexeを`legacy`で8秒実行。終了コード0、記録対象の復号124件、表示集計82枚。回帰の短い動作確認であり性能比較ではない。

### 失敗・制約

- PowerShellの実行ポリシーにより最初のスクリプト起動が拒否された。許可を得て、該当プロセスだけExecutionPolicy Bypassでビルドした。全体のポリシー設定は変更していない。
- テスト初版のinitializer list型推論エラー、GUIDの型変換警告を修正した。
- 画面起動補助の初版でPythonにないSW_SHOW属性を参照して失敗。Win32の値5へ修正し、別run名で再実行した。
- ビルド時にpwsh.exe不在のメッセージあり。MSBuildは0終了し、実行検証も成功。
- computer-useで画面目視確認を試みたが、`Computer Use native pipe is unavailable ... (os error 2)`で接続失敗。再初期化でも回復しなかった。画面の文字切れ・配置・閉じる操作の目視検証は未完了と明記した。
- G0の撮影/復号時刻の誤対応は未修正。G1-03の必須修正として維持した。
- task/seedは現段階では識別ラベル。物理・映像・制御は未接続。全summaryのtask_resultはnot_run、closed_loop_validatedはfalse。

### 証拠と現在地

証拠は`artifacts/reach_g1_20260921/`の`build_Release*`、`tests_build_fixed.log`、`unit_test_result.txt`、`unit_sessions_final/`、`verification/integration_01/`、`verification/legacy_smoke_01/`、`runs/foundation_smoke_01/`、`runs/foundation_ui_02/`。同フォルダはgit対象外。

最終確認ではPython構文、プロジェクトXML、設定JSON、文書参照とコードフェンス、38タスクの一意性・完了8件を確認した。統合/既存モード試験のexeハッシュも現バイナリと一致。`git diff --check`はエラーなし。結果は`final_checks.json`に保存した。

G1-01を完了へ更新。G1は1/6項目完了、研究ゲートは引き続きG0のみ1/7。次はG1-02の世界・運動・真値評価器と、G1-03の撮影/符号化/復号対応。仕様はv0.1.2として基盤実装への参照と結果の意味を追記した。

## E006 — 2026-09-21：仮想ロボット接続と撮影/復号対応修正

対象ID：G1-02、G1-03。着手時は未着手。ユーザーの指示に従い、前進・制動で接続を検証する範囲とし、画像による自動制御はG1-04へ残した。

### 実装と意図

- `ReachRobotWorld.*`に100 Hzの運動、加減速、T1/T2の環境・真値評価器、透視投影カメラを追加。同じ初期状態からの再構築をリセットとし、固定刻みで軌跡を再現する。
- `ReachRobotVideo.*`に独立したencoderワーカーと、H.264/RNVP v3/localhost UDP/既存decoderの接続を追加。画像に番号を描き込み、受信画素との一致を確認する。
- `FrameIdentityLedger.h`とH264Encoderの追跡API、NetworkVideoReceiverのPTS照合を追加。受理した入力だけを台帳へ登録し、出力PTSで元の撮影情報へ戻す。現在入力の撮影情報による上書きを廃止する。
- 研究経路に欠番検出・flush・IDR待機、最大32 AUの順序付き受信キュー、4 MiB受信バッファ、selectによる待機を追加。既存モードのキュー設定・受信タイムアウトは従来のまま。
- encoder/decoderの終了時drain、形式変更時の正しい出力形式選択、NV12格納高の扱い、COM/MFのワーカー単位管理を追加。同期・非同期の待ちを観測値から消さない。
- Workbenchに受信映像・真値マップ・照合枚数・撮影→復号時間を追加。ユーザー画像で切れていた保存先を、横スクロール可能な読み取り専用欄へ変更した。
- 実行用robot JSON、起動cmd、単体テスト、実映像受入スクリプト、実装説明書を追加。既存のPython起動器はstage選択と実行ごとの診断ログ分離へ拡張した。

### 検証中に見つけた問題と処置

1. `robot_loss_01`：１ AU欠落後、復号APIは成功しても24枚の画像番号が読めなかった。これを成功にせず、研究経路に欠番後の参照破棄とIDR待機を追加した。
2. `acceptance_01`：非同期encoderで形式変更の直後に出力を再要求し、E_UNEXPECTEDを検出した。次のHaveOutputイベントまで待つよう修正した。
3. `acceptance_02` / `robot_decoder_init_01`：decoderの出力形式を手作りし直すと、起動時の再交渉に失敗して先頭44枚が出なかった。MFTの提示する実形式を使用し、格納高と表示高を区別した。`robot_decoder_init_02`は６枚全一致。
4. `acceptance_03`：0.1秒のsoftware試験で、末尾のまとまった出力を既存の最新優先キューが間引いた。研究経路を最大32 AUのFIFOに変更し、二つのスレッドによる取り出し順も保護した。
5. `acceptance_04/05`：意図的な損失を設定していないのに単一パケット欠番を観測した。バッファ拡大だけでは解消しなかった。`packet_audit_01`で送信247/受信246、seq 15の不着を送受信境界のログで確認。研究経路のSO_RCVTIMEOを無効にし、selectで到着を待つ方式へ変更した後、`packet_audit_02`は247/247、後述の最終試験も合格した。OS内部の喪失原因を断定したものではない。
6. パケット診断の初版でgetenvの非推奨をエラー扱いするビルド設定に抵触。GetEnvironmentVariableAへ変更した。pwsh.exe不在の既存メッセージは残るが、最終MSBuildは終了コード0。

途中の失敗を含む証拠は削除していない。最終合格は同一バイナリの`acceptance_06`で判定した。

### 最終結果

- Release x64ビルド成功。exe SHA-256：`c1a7a3f9fc28f1cd90a172601ea9661eed6693ebcb1795c6f352100a43db66d3`。
- 世界・台帳の単体280アサーション、基盤の単体663チェック、基盤実アプリ12ケースに合格。
- 映像受入６ケース：15秒autoで450枚、４秒softwareで120枚、初期状態変更/リセットの３秒autoを２回各90枚、0.1秒softwareで３枚、AU欠落後のIDR復帰で64枚。合計817枚すべてで画像番号・stream・撮影時刻・出力PTSが一致。意図しないパケット欠番とUDPキュー破棄は0。
- software試験は終了時に16枚、短いsoftware試験は全３枚をencoderから排出した。decoder末尾出力も検証に含めた。
- 既存モード８秒：終了0、記録対象の復号140件、表示95枚。性能比較ではない。
- 新画面`robot_ui_final_01`は15秒完走し、450枚すべて一致、UDP欠番0。完了画面をユーザーが閉じるまで残した。
- 保存BMPの受信画素を確認。Windows画面確認ツールは再試行しても`native pipe is unavailable (os error 2)`で失敗したため、新画面全体の文字切れ・配置・操作の目視検証は未完了。

### 証拠・現在地

`artifacts/reach_g1_robot_20260921/`の`build_Release*`、`unit_report.json`、`packet_audit_comparison.json`、`verification/acceptance_06/report.json`、`verification/foundation_regression_01/report.json`、`verification/legacy_regression_01/report.json`、`runs/robot_ui_final_01/`を参照する。詳細は`docs/Reach_RT_G1_Robot_Video.md`。

G1-02/03を完了とした。G1は3/6項目、全タスク10/38項目完了、研究ゲートはG0のみ1/7。specはv0.1.3。次はG1-04の画像認識・姿勢推定・固定制御器、続いて指令UDPと理想回線18初期姿勢の確認。自動作業成功と研究効果は未検証である。

## E007 — 2026-09-21：報告形式と説明方針の明文化

ユーザーから「毎回、現在の状況と次工程を明確にし、実装内容もプロの場で説明できるよう分かりやすく説明する」と指定された。

進捗管理資料の2章に、現在地→実装内容と目的→検証結果と限界→次工程の報告形式を追加した。各機能の入力・処理・出力、具体的な動作の変化、研究上の役割を説明し、次工程にはタスクIDと完了条件を添える。冒頭の仕様書バージョン表記を実際のv0.1.3へ修正した。

今回は報告方法の整備で、実装コード・実験結果・タスク完了数は変更していない。現在地はG1-01～03完了、G1は3/6項目、研究ゲートはG0のみ1/7。次工程はG1-04。真値を入力にせず、受信画素からの推定・指令生成・観測喪失時の扱いを確認する。

## 今後の追記テンプレート

```text
## E番号 — YYYY-MM-DD：作業名

対象ID：
着手時の状態：
行ったこと：
変更したファイル・成果物：
検証したコード状態・設定：
実行したコマンド / ログの保存先：
結果：
未確認事項・失敗・阻害要因：
タスク状態の変更：
仕様変更の有無と理由：
次に行うこと：
```
