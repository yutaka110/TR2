#pragma once
#include <cstdint>
#include <vector>

// NALメタ（Annex-Bスキャン結果）
struct NalInfo {
  uint8_t type;   // 1..23 (H.264)
  size_t sc_pos;  // start code 位置
  size_t sc_len;  // 3 or 4
  size_t hdr_pos; // NALヘッダ位置 (= sc_pos+sc_len)
  size_t end_pos; // 次のstart code直前 or 終端
  size_t payload; // ヘッダ含むサイズ (end_pos - hdr_pos)
};

// Annex-Bをスキャンして NAL 配列にする
std::vector<NalInfo> ScanAnnexB(const std::vector<uint8_t> &v);

// フレーム種別を返す（"IDR"/"I"/"P"/"B"/"SPS"/"PPS"/"SEI"/"UNK"）
const char *DetectFrameType(const std::vector<uint8_t> &frame);