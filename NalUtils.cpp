#include "NalUtils.h"
#include <cstdio>

static inline bool IsSC3(const std::vector<uint8_t> &v, size_t k) {
  return k + 2 < v.size() && v[k] == 0 && v[k + 1] == 0 && v[k + 2] == 1;
}
static inline bool IsSC4(const std::vector<uint8_t> &v, size_t k) {
  return k + 3 < v.size() && v[k] == 0 && v[k + 1] == 0 && v[k + 2] == 0 &&
         v[k + 3] == 1;
}
static size_t FindNextSC(const std::vector<uint8_t> &v, size_t from) {
  for (size_t k = from; k + 2 < v.size(); ++k)
    if (IsSC3(v, k) || IsSC4(v, k))
      return k;
  return v.size();
}

std::vector<NalInfo> ScanAnnexB(const std::vector<uint8_t> &v) {
  std::vector<NalInfo> out;
  const size_t n = v.size();
  size_t p = FindNextSC(v, 0);
  while (p < n) {
    size_t sc_len = IsSC3(v, p) ? 3 : 4;
    size_t hdr = p + sc_len;
    size_t q = FindNextSC(v, hdr + 1);
    if (hdr < n) {
      uint8_t nal = v[hdr] & 0x1F; // H.264
      NalInfo ni{};
      ni.type = nal;
      ni.sc_pos = p;
      ni.sc_len = sc_len;
      ni.hdr_pos = hdr;
      ni.end_pos = (q <= n) ? q : n;
      ni.payload = (ni.end_pos > hdr) ? (ni.end_pos - hdr) : 0;
      out.push_back(ni);
    }
    p = (q < n) ? q : n;
  }
  return out;
}

// 00 00 03 のエミュ防止バイト除去
static void UnescapeRbsp(const uint8_t *src, size_t len,
                         std::vector<uint8_t> &out) {
  out.clear();
  out.reserve(len);
  for (size_t i = 0; i < len; ++i) {
    if (i + 2 < len && src[i] == 0 && src[i + 1] == 0 && src[i + 2] == 3) {
      out.push_back(0);
      out.push_back(0);
      i += 2; // 0x03 をスキップ
    } else {
      out.push_back(src[i]);
    }
  }
}

// シンプルビットリーダ & ue(v)
struct BitReader {
  const uint8_t *p;
  size_t n;
  size_t bit;
  BitReader(const uint8_t *pb, size_t nb) : p(pb), n(nb), bit(0) {}
  bool get1(uint32_t &v) {
    if (bit >= n * 8)
      return false;
    v = (p[bit >> 3] >> (7 - (bit & 7))) & 1;
    ++bit;
    return true;
  }
  bool getn(int k, uint32_t &v) {
    v = 0;
    for (int i = 0; i < k; i++) {
      uint32_t b;
      if (!get1(b))
        return false;
      v = (v << 1) | b;
    }
    return true;
  }
};
static bool read_ue(BitReader &br, uint32_t &out) {
  int leadingZeroBits = -1;
  uint32_t b = 0;
  do {
    if (!br.get1(b))
      return false;
    ++leadingZeroBits;
  } while (!b);
  if (leadingZeroBits == 0) {
    out = 0;
    return true;
  }
  uint32_t suffix = 0;
  if (!br.getn(leadingZeroBits, suffix))
    return false;
  out = ((1u << leadingZeroBits) - 1u) + suffix;
  return true;
}

const char *DetectFrameType(const std::vector<uint8_t> &frame) {
  auto nals = ScanAnnexB(frame);

  // まずは非VCLを拾って早期リターン（ログ用途）
  for (const auto &ni : nals) {
    switch (ni.type) {
    case 7:
      return "SPS";
    case 8:
      return "PPS";
    case 6:
      return "SEI";
    default:
      break;
    }
  }

  for (const auto &ni : nals) {
    if (ni.type == 5)
      return "IDR"; // I(IDR)確定
    if (ni.type != 1)
      continue; // VCL以外はスキップ

    // non-IDR の slice_type を読む
    const uint8_t *payload = frame.data() + ni.hdr_pos + 1;
    const size_t payloadLen =
        (ni.end_pos > ni.hdr_pos + 1) ? (ni.end_pos - (ni.hdr_pos + 1)) : 0;
    if (payloadLen == 0)
      continue;

    std::vector<uint8_t> rbsp;
    UnescapeRbsp(payload, payloadLen, rbsp);

    BitReader br(rbsp.data(), rbsp.size());
    uint32_t tmp = 0, slice_type = 0;
    if (!read_ue(br, tmp))
      continue; // first_mb_in_slice
    if (!read_ue(br, slice_type))
      continue; // slice_type

    switch (slice_type % 5) { // 0=P,1=B,2=I,3=SP,4=SI
    case 2:
      return "I";
    case 0:
      return "P";
    case 1:
      return "B";
    default:
      return "UNK";
    }
  }
  return "UNK";
}