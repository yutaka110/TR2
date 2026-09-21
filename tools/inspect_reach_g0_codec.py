"""Inspect a bounded subset of H.264 Annex-B syntax for the G0 probe.

This is NOT a conformance validator or a complete reference-picture parser.
Unknown/truncated syntax is reported rather than accepted as a simple IP chain.
"""
from __future__ import annotations
import argparse
from collections import Counter
import json
from pathlib import Path
import re


class Bits:
    def __init__(self, data: bytes):
        self.data, self.pos = data, 0

    def take(self, count: int = 1) -> int:
        if count < 0 or count > 32 or self.pos + count > len(self.data) * 8:
            raise ValueError("truncated or unsupported bit field")
        result = 0
        for _ in range(count):
            result = (result << 1) | ((self.data[self.pos // 8] >> (7 - self.pos % 8)) & 1)
            self.pos += 1
        return result

    def ue(self) -> int:
        zeros = 0
        while not self.take():
            zeros += 1
            if zeros > 30:
                raise ValueError("Exp-Golomb value exceeds probe limit")
        return (1 << zeros) - 1 + self.take(zeros)

    def se(self) -> int:
        n = self.ue()
        return (n + 1) // 2 if n & 1 else -(n // 2)


def rbsp(nal: bytes) -> bytes:
    return re.sub(b"\x00\x00\x03", b"\x00\x00", nal[1:])


def split_nals(data: bytes) -> list[bytes]:
    starts = list(re.finditer(b"\x00\x00(?:\x00)?\x01", data))
    if not starts or any(data[:starts[0].start()]):
        raise ValueError("not an Annex-B AU")
    result = [data[m.end():starts[i + 1].start() if i + 1 < len(starts) else len(data)]
              for i, m in enumerate(starts)]
    if any(not n or n[0] & 128 for n in result):
        raise ValueError("empty NAL or forbidden_zero_bit")
    return result


def parse_sps(nal: bytes) -> dict:
    b = Bits(rbsp(nal))
    profile, constraints, level, sid = b.take(8), b.take(8), b.take(8), b.ue()
    chroma = 1
    if profile in {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135}:
        chroma = b.ue()
        if chroma == 3:
            raise ValueError("separate colour plane profile unsupported by probe")
        b.ue(); b.ue(); b.take()
        if b.take():
            raise ValueError("scaling matrix unsupported by probe")
    frame_num_bits = b.ue() + 4
    poc_type = b.ue()
    if poc_type == 0:
        b.ue()
    elif poc_type == 1:
        b.take(); b.se(); b.se()
        cycle = b.ue()
        if cycle > 256:
            raise ValueError("POC cycle exceeds probe limit")
        for _ in range(cycle): b.se()
    elif poc_type != 2:
        raise ValueError("invalid pic_order_cnt_type")
    max_refs, gaps = b.ue(), b.take()
    width_mbs, height_map_units = b.ue() + 1, b.ue() + 1
    frame_only = b.take()
    if not frame_only: b.take()
    b.take()
    crop = [b.ue() for _ in range(4)] if b.take() else [0, 0, 0, 0]
    # The production experiment uses 4:2:0, progressive NV12.
    if chroma != 1 or frame_only != 1:
        raise ValueError("only progressive 4:2:0 dimensions supported by probe")
    return dict(sps_id=sid, profile_idc=profile, constraints=constraints, level_idc=level,
                frame_num_bits=frame_num_bits, poc_type=poc_type, max_num_ref_frames=max_refs,
                gaps_allowed=bool(gaps), frame_mbs_only=bool(frame_only),
                width=width_mbs * 16 - 2 * (crop[0] + crop[1]),
                height=height_map_units * 16 - 2 * (crop[2] + crop[3]))


def inspect(directory: Path) -> dict:
    sequences, records, errors = {}, [], []
    for path in sorted((directory / "aus").glob("*.h264")):
        record = {"output_index": int(path.stem), "bytes": path.stat().st_size, "nal_types": [], "slices": []}
        try:
            for nal in split_nals(path.read_bytes()):
                kind = nal[0] & 31
                record["nal_types"].append(kind)
                if kind == 7:
                    sps = parse_sps(nal)
                    sequences[json.dumps(sps, sort_keys=True)] = sps
                elif kind in (1, 5):
                    b = Bits(rbsp(nal))
                    first_mb, slice_type, pps = b.ue(), b.ue(), b.ue()
                    if slice_type > 9:
                        raise ValueError("invalid slice_type")
                    record["slices"].append(dict(first_mb=first_mb, type=("P", "B", "I", "SP", "SI")[slice_type % 5],
                                                  slice_type=slice_type, pps_id=pps,
                                                  idr=kind == 5, nal_ref_idc=(nal[0] >> 5) & 3))
            if not record["slices"]:
                raise ValueError("no VCL slice in AU")
        except ValueError as error:
            errors.append({"file": path.name, "error": str(error)})
        records.append(record)
    summary = {
        "access_units": len(records), "parse_errors": errors,
        "sps_variants": list(sequences.values()),
        "idr_output_indices": [r["output_index"] for r in records if 5 in r["nal_types"]],
        "slice_type_counts": dict(Counter(s["type"] for r in records for s in r["slices"])),
        "reference_graph_verified": False,
        "limitation": "PPS reference-list changes, MMCO, VUI and output PTS identity are not validated.",
    }
    (directory / "syntax_records.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
    (directory / "syntax_summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    return summary


def self_check() -> None:
    assert [Bits(bytes([x])).ue() for x in [128, 64, 96]] == [0, 1, 2]
    assert [Bits(bytes([x])).se() for x in [128, 64, 96]] == [0, 1, -1]
    assert split_nals(b"\x00\x00\x00\x01\x67\x80\x00\x00\x01\x65\x80") == [b"\x67\x80", b"\x65\x80"]
    assert rbsp(b"\x67\x00\x00\x03\x01") == b"\x00\x00\x01"
    for malformed in (b"", b"\x00", b"\x00\x00\x00\x00"):
        try: Bits(malformed).ue()
        except ValueError: pass
        else: raise AssertionError("truncated Exp-Golomb was accepted")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    self_check()
    result = inspect(args.directory)
    print(json.dumps(result, indent=2))
    raise SystemExit(1 if result["parse_errors"] or not result["access_units"] else 0)
