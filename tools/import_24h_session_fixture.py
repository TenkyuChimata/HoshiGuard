#!/usr/bin/env python3
"""Deterministically import a compact fixture from the real 24-hour soak."""

from __future__ import annotations

import argparse
from hashlib import sha256
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

from hoshiguard.bridge_frames import FrameType, Status, parse_payload  # noqa: E402
from hoshiguard.bridge_protocol import crc32_iso_hdlc  # noqa: E402
from hoshiguard.events import EventProcessor  # noqa: E402
from hoshiguard.l1s import compare_claim  # noqa: E402
from hoshiguard.output.raw_archive import (  # noqa: E402
    FILE_HEADER, FILE_MAGIC, RECORD_MAGIC, RawArchiveReader)
from hoshiguard.routing import make_alert  # noqa: E402
from hoshiguard.session_validation import validate_session  # noqa: E402

DESTINATION = ROOT / "test/fixtures/bridge_24h_session"
NS = 1_000_000_000


def archive_bytes(records) -> bytes:
    output = bytearray(FILE_HEADER.pack(FILE_MAGIC, 1, 0, 0))
    for record in records:
        body = struct.pack("<QI", record.pc_receive_ns, len(record.wire)) + record.wire
        output.extend(RECORD_MAGIC)
        output.extend(struct.pack("<I", len(body) + 4))
        output.extend(body)
        output.extend(struct.pack("<I", crc32_iso_hdlc(body)))
    return bytes(output)


def build(source: Path) -> dict[str, str | bytes]:
    selected: dict[int, tuple[object, set[str]]] = {}
    first_pc = None
    first_by_type = {}
    last_by_type = {}
    first_mt = {}
    first_sv = {}
    first_pab = {}
    first_hour = {}
    dcr_categories = {}
    dcx_variants = {}
    actions = {}
    states = {}
    queue_hwm = -1
    previous_status_record = None
    previous_l1s = None
    maximum_l1s_pair = None
    maximum_l1s_gap = -1
    event_processor = EventProcessor()

    def add(record, reason: str) -> None:
        if record is None:
            return
        if record.offset not in selected:
            selected[record.offset] = (record, set())
        selected[record.offset][1].add(reason)

    for record in RawArchiveReader(str(source)):
        if first_pc is None:
            first_pc = record.pc_receive_ns
        frame_type = record.frame.frame_type
        first_by_type.setdefault(frame_type, record)
        last_by_type[frame_type] = record
        hour = int((record.pc_receive_ns - first_pc) // (3600 * NS))
        if hour not in first_hour:
            first_hour[hour] = record
        try:
            typed = parse_payload(record.frame)
        except ValueError:
            continue
        if isinstance(typed, Status):
            if typed.queue_high_watermark > queue_hwm:
                add(previous_status_record, "before queue high-watermark increase")
                add(record, f"queue high-watermark became {typed.queue_high_watermark}")
                queue_hwm = typed.queue_high_watermark
            previous_status_record = record
            states.setdefault((typed.pps_state, typed.time_quality), record)
        if frame_type != FrameType.L1S_FRAME:
            continue
        if previous_l1s is not None:
            gap = record.pc_receive_ns - previous_l1s.pc_receive_ns
            if gap > maximum_l1s_gap:
                maximum_l1s_gap = gap
                maximum_l1s_pair = (previous_l1s, record)
        previous_l1s = record
        claim = typed
        first_mt.setdefault(claim.message_type, record)
        first_sv.setdefault(claim.sv_id, record)
        first_pab.setdefault(claim.pab, record)
        comparison = compare_claim(claim)
        if comparison.verified is None or comparison.mismatch:
            continue
        alert = make_alert(record.frame, claim, comparison.verified)
        result = event_processor.observe(alert)
        if alert.service.value == "DCR":
            dcr_categories.setdefault(alert.category.code, record)
        if alert.service.value == "DCX":
            variant = getattr(alert.payload, "extended", None)
            dcx_variants.setdefault(getattr(variant, "variant", "unknown"), record)
        if result.disposition in ("issued", "updated", "cancelled", "cleared"):
            actions.setdefault(result.disposition, record)

    for kind, record in first_by_type.items():
        add(record, f"first {FrameType(kind).name}")
    for kind, record in last_by_type.items():
        add(record, f"last {FrameType(kind).name}")
    for hour, record in sorted(first_hour.items()):
        add(record, f"hour {hour} representative")
    for mt, record in sorted(first_mt.items()):
        add(record, f"first MT{mt}")
    for sv, record in sorted(first_sv.items()):
        add(record, f"first svId {sv}")
    for pab, record in sorted(first_pab.items()):
        add(record, f"first PAB {pab:02X}")
    for category, record in sorted(dcr_categories.items()):
        add(record, f"first DCR category {category}")
    for variant, record in sorted(dcx_variants.items()):
        add(record, f"first DCX type {variant}")
    for action, record in sorted(actions.items()):
        add(record, f"first event action {action}")
    for state, record in sorted(states.items()):
        add(record, f"first PPS/time state {state[0]}/{state[1]}")
    if maximum_l1s_pair:
        add(maximum_l1s_pair[0], "before maximum L1S gap")
        add(maximum_l1s_pair[1], "after maximum L1S gap")

    ordered = [selected[key] for key in sorted(selected)]
    summary = validate_session(str(source))
    metadata = []
    for index, (record, reasons) in enumerate(ordered):
        item = {"selected_index": index, "source_offset": record.offset,
                "pc_receive_ns": record.pc_receive_ns,
                "sequence": record.frame.sequence,
                "frame_type": FrameType(record.frame.frame_type).name,
                "reasons": sorted(reasons),
                "wire_sha256": sha256(record.wire).hexdigest()}
        if record.frame.frame_type == FrameType.L1S_FRAME:
            claim = parse_payload(record.frame)
            item.update({"mt": claim.message_type, "sv_id": claim.sv_id,
                         "pab": f"{claim.pab:02X}"})
        metadata.append(item)
    manifest = {
        "schema": "hoshiguard.bridge_24h_fixture.v1",
        "source": "real", "evidence_source": "real_hardware",
        "source_session": {"path": str(source), "sha256": summary["source"]["sha256"],
                           "size_bytes": source.stat().st_size,
                           "start_utc": summary["start_utc"],
                           "end_utc": summary["end_utc"],
                           "firmware": summary["hello"]["first"]["firmware"],
                           "bridge_protocol": "1.0"},
        "selection_rule": "first deterministic evidence plus hourly representatives",
        "selected_records": metadata,
        "maximum_l1s_gap_seconds": maximum_l1s_gap / NS,
        "privacy": {"coordinates_retained": False, "nav_pvt_retained": False},
    }
    expected = {
        "source_sha256": summary["source"]["sha256"],
        "records": summary["hgraw_records"],
        "frame_type_distribution": summary["frame_type_distribution"],
        "sequence": summary["sequence"], "l1s": summary["l1s"],
        "status": summary["status"],
        "zero_l1s_drop_proven": summary["zero_l1s_drop_proven"],
    }
    readme = f"""# HoshiGuard real 24-hour Bridge fixture

Deterministically selected from real-hardware firmware 0.2.5 session
`{summary['source']['sha256']}`. The 27 MB source is not copied. Exact wire
records cover both ends, each observed frame/MT/satellite/PAB, every hour,
queue high-watermark changes, actual DCR/DCX variants and the maximum L1S gap.

This is inherited Bridge 1.0/path evidence, not a claim that firmware 0.2.7
itself has completed a 24-hour hardware soak. No NAV-PVT or coordinates exist.
"""
    return {
        "manifest.json": json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        "expected_summary.json": json.dumps(expected, indent=2, sort_keys=True) + "\n",
        "selected_records.hgraw": archive_bytes(item[0] for item in ordered),
        "README.md": readme,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    artifacts = build(Path(args.session))
    if args.check:
        mismatched = [name for name, value in artifacts.items()
                      if not (DESTINATION / name).exists() or
                      (DESTINATION / name).read_bytes() !=
                      (value.encode() if isinstance(value, str) else value)]
        if mismatched:
            print("24h fixture mismatch: " + ", ".join(mismatched), file=sys.stderr)
            return 1
        print("24h real-session fixture check OK")
        return 0
    DESTINATION.mkdir(parents=True, exist_ok=True)
    for name, value in artifacts.items():
        (DESTINATION / name).write_bytes(value.encode() if isinstance(value, str)
                                         else value)
    print(f"wrote {len(artifacts)} artifacts to {DESTINATION}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
