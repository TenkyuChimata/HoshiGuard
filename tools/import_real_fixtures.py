#!/usr/bin/env python3
"""現場取得からプライバシーに配慮した QZSS fixture を決定的に抽出する。"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib
import sys
from dataclasses import dataclass
from typing import Optional

import capture_io
import replay_ubx

ROOT = pathlib.Path(__file__).resolve().parents[1]
REAL_DIR = ROOT / "test" / "fixtures" / "real"
PACKETS = ROOT / "test" / "fixtures" / "packets.h"
ARRIVALS = REAL_DIR / "real_capture_arrivals.h"


@dataclass(frozen=True)
class Selected:
    name: str
    record: replay_ubx.UbxRecord
    source: str
    expected_accept: bool
    expected_l1s: bool
    frame: Optional[replay_ubx.L1sRecord] = None
    note: str = ""


def _rechecksum(packet: bytearray) -> bytes:
    a, b = replay_ubx.ubx_checksum(bytes(packet[2:-2]))
    packet[-2] = a
    packet[-1] = b
    return bytes(packet)


def packet_header(items) -> str:
    """実 packet と合成負例を単一ヘッダーにまとめ、元の byte 列を保持する。"""
    arrays, rows = [], []
    for name, packet, accept, l1s, pab, mt, words in items:
        symbol = _symbol(name)
        values = ", ".join(f"0x{value:02X}" for value in packet)
        arrays.append(f"static const uint8_t {symbol}[] = {{{values}}};\n")
        rows.append(f'    {{"{name}", hoshiguard_fixture::{symbol}, '
                    f'sizeof(hoshiguard_fixture::{symbol}), '
                    f'{str(accept).lower()}, {str(l1s).lower()}, {pab}U, {mt}U, {words}U}},\n')
    return (
        "#pragma once\n\n#include <stddef.h>\n#include <stdint.h>\n\n"
        "// 出所・実受信と合成負例の区別は real/manifest.json を参照。\n"
        "namespace hoshiguard_fixture {\n" + "".join(arrays) + "}\n\n"
        "struct RealFixture {\n  const char *name;\n  const uint8_t *data;\n"
        "  size_t length;\n  bool expectedUbxAccept;\n  bool expectedL1s;\n"
        "  uint8_t pab;\n  uint8_t messageType;\n  uint8_t numWords;\n};\n\n"
        "static const RealFixture kRealFixtures[] = {\n" + "".join(rows) +
        "};\nstatic const size_t kRealFixtureCount = "
        "sizeof(kRealFixtures) / sizeof(kRealFixtures[0]);\n")


def _symbol(name: str) -> str:
    return "k" + "".join(part.capitalize() for part in name.split("_"))


def select(records: list[replay_ubx.UbxRecord]) -> tuple[list[Selected], list[int]]:
    seen: dict[bytes, int] = {}
    frames: list[tuple[replay_ubx.UbxRecord, replay_ubx.L1sRecord]] = []
    l1ca: Optional[replay_ubx.UbxRecord] = None
    for record in sorted(records, key=lambda item: (item.timestamp_us, item.line_number)):
        header = replay_ubx.parse_sfrbx(record.packet)
        if header and header.gnss_id == 5 and header.sig_id == 0 and l1ca is None:
            l1ca = record
        frame = replay_ubx.extract_l1s(record, seen)
        if frame and frame.valid_pab and frame.valid_crc:
            frames.append((record, frame))

    selected: list[Selected] = []
    targets = [
        ("qzss_l1s_mt43_pab53", 43, 0x53),
        ("qzss_l1s_mt43_pab9a", 43, 0x9A),
        ("qzss_l1s_mt43_pabc6", 43, 0xC6),
        ("qzss_l1s_mt44_pab53", 44, 0x53),
        ("qzss_l1s_mt44_pab9a", 44, 0x9A),
        ("qzss_l1s_mt44_pabc6", 44, 0xC6),
        ("qzss_l1s_mt47", 47, None),
        ("qzss_l1s_mt48", 48, None),
        ("qzss_l1s_mt49", 49, None),
        ("qzss_l1s_mt50", 50, None),
    ]
    for name, mt, pab in targets:
        match = next(((record, frame) for record, frame in frames
                      if frame.message_type == mt and (pab is None or frame.pab == pab)), None)
        if match is None:
            raise ValueError(f"capture has no valid fixture for {name}")
        selected.append(Selected(name, match[0], "real", True, True, match[1]))
    if l1ca is None:
        raise ValueError("capture has no complete QZSS L1C/A UBX record")
    selected.append(Selected("qzss_l1ca_negative", l1ca, "real", True, False,
                             note="real QZSS sigId=0; must not enter L1S"))

    base = bytearray(selected[0].record.packet)
    non_qzss = bytearray(l1ca.packet)
    non_qzss[6] = 0  # 合成した GPS gnssId。元の完全 packet は実受信 QZSS。
    non_qzss_packet = _rechecksum(non_qzss)
    selected.append(Selected(
        "non_qzss_negative",
        replay_ubx.UbxRecord(l1ca.timestamp_us, non_qzss_packet, 0,
                             "synthetic-derived"),
        "synthetic-derived", True, False,
        note="gnssId changed to GPS and UBX checksum recomputed"))

    corrupted = bytearray(base)
    corrupted[-1] ^= 0x01
    selected.append(Selected(
        "ubx_checksum_corrupted",
        replay_ubx.UbxRecord(selected[0].record.timestamp_us, bytes(corrupted),
                             0, "synthetic-corrupted"),
        "synthetic-corrupted", False, False,
        note="one UBX checksum bit flipped; parser rejection fixture"))

    mismatch = bytearray(base)
    mismatch[10] = 8  # SFRBX numWords のみを変更し、payload は実受信の 9 word 長を保持する。
    mismatch_packet = _rechecksum(mismatch)
    selected.append(Selected(
        "sfrbx_length_mismatch",
        replay_ubx.UbxRecord(selected[0].record.timestamp_us, mismatch_packet,
                             0, "synthetic-derived"),
        "synthetic-derived", True, False,
        note="numWords changed from 9 to 8; UBX checksum recomputed"))
    return selected, [record.timestamp_us for record, _ in frames]


def render(capture: capture_io.CaptureInput, selected: list[Selected],
           arrivals: list[int], pps_arrivals: list[int], capture_date: str,
           firmware: str) -> dict[pathlib.Path, str]:
    files: dict[pathlib.Path, str] = {}
    manifest_items = []
    for item in selected:
        header = replay_ubx.parse_sfrbx(item.record.packet)
        frame = item.frame
        manifest_items.append({
            "name": item.name,
            "header": "../packets.h",
            "source": item.source,
            "source_capture_sha256": capture.sha256,
            "capture_date": capture_date,
            "max_m10s_firmware": firmware,
            "note": item.note,
            "message_nature": "not semantically verified",
            "manual_field_review": False,
            "timestamp_us": item.record.timestamp_us,
            "ubx_hex": item.record.packet.hex().upper(),
            "ubx_length": len(item.record.packet),
            "ubx_sha256": __import__("hashlib").sha256(item.record.packet).hexdigest(),
            "expected_ubx_accept": item.expected_accept,
            "expected_l1s": item.expected_l1s,
            "gnss_id": header.gnss_id if header else None,
            "sv_id": header.sv_id if header else None,
            "sig_id": header.sig_id if header else None,
            "num_words": header.num_words if header else None,
            "pab": frame.pab if frame else None,
            "message_type": frame.message_type if frame else None,
            "received_crc": f"{frame.received_crc:06X}" if frame else None,
            "calculated_crc": f"{frame.calculated_crc:06X}" if frame else None,
            "duplicate": frame.duplicate if frame else None,
            "extra_words_hex": frame.extra_words.hex().upper() if frame else None,
            "region": "withheld",
            "coordinates": "not retained",
        })
    manifest = {
        "schema": 1,
        "source_capture": capture.path.name,
        "source_capture_sha256": capture.sha256,
        "capture_date": capture_date,
        "max_m10s_firmware": firmware,
        "region": "withheld",
        "coordinates": "not retained",
        "selection_rule": "first checksum-valid record by timestamp satisfying each target",
        "fixtures": manifest_items,
    }
    files[REAL_DIR / "manifest.json"] = json.dumps(
        manifest, indent=2, sort_keys=True) + "\n"

    files[PACKETS] = packet_header(
        (item.name, item.record.packet, item.expected_accept, item.expected_l1s,
         item.frame.pab if item.frame else 0,
         item.frame.message_type if item.frame else 0,
         item.frame.num_words if item.frame else 0)
        for item in selected)
    arrival_rows = []
    for index in range(0, len(arrivals), 6):
        arrival_rows.append("    " + ", ".join(f"{value}ULL" for value in arrivals[index:index + 6]) + ",\n")
    files[ARRIVALS] = (
        "#pragma once\n\n#include <stddef.h>\n#include <stdint.h>\n\n"
        "static const uint64_t kRealL1sArrivalTimestampUs[] = {\n" +
        "".join(arrival_rows) +
        "};\nstatic const size_t kRealL1sArrivalCount = "
        "sizeof(kRealL1sArrivalTimestampUs) / sizeof(kRealL1sArrivalTimestampUs[0]);\n\n"
        "static const uint32_t kRealPpsTimestampUs[] = {\n" +
        "".join("    " + ", ".join(f"{value}UL" for value in pps_arrivals[index:index + 8]) + ",\n"
                for index in range(0, len(pps_arrivals), 8)) +
        "};\nstatic const size_t kRealPpsTimestampCount = "
        "sizeof(kRealPpsTimestampUs) / sizeof(kRealPpsTimestampUs[0]);\n"
    )
    return files


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--firmware", default="SPG 5.10")
    parser.add_argument("--capture-date")
    args = parser.parse_args(argv)
    try:
        capture = capture_io.read_capture(args.capture)
        records, errors = replay_ubx.parse_log(capture.lines)
        if errors:
            raise ValueError(f"capture contains {len(errors)} malformed UBX records")
        selected, arrivals = select(records)
        capture_date = args.capture_date or dt.datetime.fromtimestamp(
            args.capture.stat().st_mtime).date().isoformat()
        pps_arrivals = []
        for line in capture.lines:
            if not line.startswith("PPS timestamp_us="):
                continue
            try:
                pps_arrivals.append(int(line.split("timestamp_us=", 1)[1].split()[0]))
            except (ValueError, IndexError):
                pass
        files = render(capture, selected, arrivals, pps_arrivals,
                       capture_date, args.firmware)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    differences = []
    for path, content in files.items():
        if not path.exists() or path.read_text(encoding="utf-8") != content:
            differences.append(path)
    if args.check:
        for path in differences:
            print(f"out-of-date: {path.relative_to(ROOT)}", file=sys.stderr)
        return 1 if differences else 0
    for path, content in files.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8", newline="\n")
    print(f"capture_sha256={capture.sha256}")
    print(f"real_fixtures={sum(item.source == 'real' for item in selected)}")
    print(f"synthetic_negative_fixtures={sum(item.source != 'real' for item in selected)}")
    print(f"l1s_arrivals={len(arrivals)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
