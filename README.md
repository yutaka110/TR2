# Realtime Video Communication Engine

## Portfolio Documentation

- [RNVP v1 Protocol Specification](docs/RNVP_v1_Protocol_Spec.md)

This project includes RNVP v1, a custom UDP-based realtime video protocol with frame/chunk headers, ACK/NACK feedback, deadline-based selective retransmission, jitter-buffer control, display-deadline dropping, RTT measurement, control commands, and QoE-driven adaptive streaming.

## 概要
C++とDirectX12を用いた自作エンジン上で、リアルタイム映像通信を行うための通信基盤です。  
対戦ゲームにおいて、離れた相手の映像をゲーム画面上に低遅延で表示し、臨場感のある体験を実現することを目的に開発しています。
本プロジェクトでは、単に映像を送受信するだけでなく、遅延、ジッタ、パケットロス、フレーム欠落などを計測・可視化し、通信状態を評価しながら安定性を改善できる仕組みを重視しています。

## 主な機能
- Media Foundationによるカメラ映像取得
- JPEG形式への圧縮
- UDPによるリアルタイム送信
- 独自プロトコル RNVP によるチャンク分割送信
- フレームID、チャンク番号、シーケンス番号、送信時刻によるパケット管理
- 受信側でのフレーム再構成
- ACKによる欠落検出
- 欠落チャンクの選択的再送
- ジッタバッファによる到着間隔の揺らぎ吸収
- RTT、ジッタ、パケットロス、フレーム欠落、ビットレートの可視化
- ImGuiによる通信状態表示
- CSV / Markdown形式での実験結果出力

## システム構成
Camera
  ↓
Media Foundation Capture
  ↓
JPEG Encode
  ↓
RNVP Packetize
  ↓
UDP Send
  ↓
UDP Receive
  ↓
Frame Reassembler
  ↓
Jitter Buffer
  ↓
Decode / Texture Upload
  ↓
DirectX12 Rendering
