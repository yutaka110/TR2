# GPU ドライバ落ち（0x887A0020）原因切り分けメモ

## 0. 事実（ログから）
- 受信 → 再構成 → SPS/PPS/IDR が入ってくる（NAL 9→7→8→5）
- 「CleanPoint output (IDR decoded)」まで到達 → IDR は成功
- 直後に GPU が落ちる  
- `GetDeviceRemovedReason=0x887A0020（DXGI_ERROR_DRIVER_INTERNAL_ERROR）`  
  → ドライバ内部でクラッシュ

---

## 1. デコードと GPU 転送の切り分け
- ハードウェア支援を切ってソフト MFT で復号
- `IMFDXGIDeviceManager` を渡さず（`MFT_MESSAGE_SET_D3D_MANAGER` しない）
- 出力はシステムメモリ（NV12）
- CPU で D3D12 テクスチャへコピー（UploadHeap → Default）
- これでハングしない → 原因は D3D11↔D3D12 のインタロップ（or ドライバ相性）
- WARP で再現テスト（`D3D11CreateDevice` / `D3D12CreateDevice` を WARP に）
  → 再現しないなら物理 GPU / ドライバ固有

---

## 2. Media Foundation（H.264）側の確認
- 入力: `MF_MT_MAJOR_TYPE=Video`, `MF_MT_SUBTYPE=MFVideoFormat_H264`
- 出力: NV12（`MFVideoFormat_NV12`）
- `MF_MT_MPEG_SEQUENCE_HEADER`（SPS/PPS）渡し or Annex-B（00 00 00 01）で連結の整合性
- `ProcessInput` / `ProcessOutput` ループ
  - `NOTACCEPTING` → 出力を引き切る → `MF_E_TRANSFORM_NEED_MORE_INPUT` まで回す
- 低遅延属性（必要なら）
  - `MFT_DECODER_FINAL_VIDEO_RESOLUTION`
  - `CODECAPI_AVDecVideoAcceleration_H264`
  - `CODECAPI_AVLowLatencyMode`
- 今回は「復号OK → ドライバ落ち」なので優先度は低め

---

## 3. D3D11 — D3D12 インタロップの地雷チェック
- **同一アダプタの保証**
  - MFT に渡す D3D11 デバイスと D3D12 デバイスが同一 DXGI アダプタか確認
- **デバイスマネージャのライフタイム**
  - `IMFDXGIDeviceManager` と D3D11Device はプロセス全体で単一 & 長寿命に
  - デコード中に D3D11 デバイスをリセットしていないか
- **共有リソースの作り方**
  - D3D12: `HEAP_FLAG_SHARED`（`HEAP_FLAG_SHARED_CROSS_ADAPTER` は避ける）
  - D3D11: `OpenSharedResource1` / `OpenSharedHandle`
  - Keyed Mutex / Fence で同期
- **ID3D11On12 を使う場合**
  - `AcquireWrappedResources` → D3D11 使用 → Flush → `ReleaseWrappedResources` → D3D12 Fence 待機
- **CPU Map タイミング**
  - `Map(Staging)` 失敗 → GPU 書き込み中の可能性
  - Copy → Fence → Wait → Map の順で
- **フォーマット & ピッチ**
  - NV12: Y 面高さ + UV 面高さ(1/2)
  - ピッチ計算・アライン確認
  - State 遷移（`COPY_SOURCE` / `COPY_DEST`）忘れに注意

---

## 4. ドライバ/OS 周りの健全性チェック
- DXGI Debug Layer 有効化（D3D11 / D3D12 両方）
- 何が違反だったかログを詳細に出す
- TDR 一時延長（デバッグ限定）
- 別 GPU / ドライババージョンで再現性確認

---

## 5. 追加で仕込むと捗るログ
- **同期ログ**
  - 「Dec→D3D11 完了」「D3D11 Flush」「Fence Signal」「D3D12 Wait 完了」「Map 入る/出る」
- **`GetDeviceRemovedReason` の逐次監視**
  - API 呼び出しごとに確認
- **リソース State 遷移ログ（D3D12）**
  - Before/After を逐次記録

---

## 6. 典型パターン別・即試す修正
- **A. IDR 後の最初の P で落ちる**
  - 双方向同期（D3D11 終了→D3D12 開始、D3D12 終了→D3D11 書き込み）
- **B. Map(Staging) だけ失敗**
  - 厳密な GPU 完了待ち
  - D3D11 側でステージング完了→システムメモリ経由で D3D12 へ
- **C. 特定解像度のみ**
  - NV12 面サイズ/アライン再確認

---

## 7. 最小再現の作り方（ショートコース）
1. ソフト MFT + CPU 経由アップロードで安定確認（落ちない基準点）
2. ハード MFT + D3D11 単独描画（D3D12 を外す）
3. ハード MFT + D3D11→D3D12 インタロップ（Fence 徹底／1フレームずつ）
