#!/usr/bin/env python3
"""実 HGRAW から小さな回帰 fixture を決定的に選択する。"""

from __future__ import annotations

import argparse
from dataclasses import asdict
from hashlib import sha256
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

from hoshiguard.bridge_frames import FrameType, parse_payload
from hoshiguard.bridge_protocol import (
    BridgeFrame, cobs_decode, cobs_encode, crc32_iso_hdlc, decode_wire,
    encode_frame,
)
from hoshiguard.l1s import verify_ubx_l1s
from hoshiguard.output.raw_archive import (
    FILE_HEADER, FILE_MAGIC, RECORD_MAGIC, RawArchiveReader,
)
from hoshiguard.session_validation import validate_session
from hoshiguard.ubx import checksum as ubx_checksum

DESTINATION = ROOT / "test/fixtures/bridge_real_session"


def archive_bytes(records) -> bytes:
    output = bytearray(FILE_HEADER.pack(FILE_MAGIC, 1, 0, 0))
    for record in records:
        body = struct.pack("<QI", record.pc_receive_ns, len(record.wire)) + record.wire
        output.extend(RECORD_MAGIC)
        output.extend(struct.pack("<I", len(body) + 4))
        output.extend(body)
        output.extend(struct.pack("<I", crc32_iso_hdlc(body)))
    return bytes(output)


def replace_payload(frame: BridgeFrame, payload: bytes,
                    sequence: int | None = None) -> bytes:
    return encode_frame(frame.frame_type,
                        frame.sequence if sequence is None else sequence,
                        frame.timestamp_us, payload, frame.flags, frame.minor)


def build(source: Path):
    first_record = last_record = first_hello = None
    first_status = last_status = first_time = last_time = None
    first_l1s = None
    mt_first = {}
    pab_first = {}
    sv_first = {}
    for record in RawArchiveReader(str(source)):
        if first_record is None:
            first_record = record
        last_record = record
        if record.frame.frame_type == FrameType.HELLO and first_hello is None:
            first_hello = record
        elif record.frame.frame_type == FrameType.STATUS:
            if first_status is None:
                first_status = record
            last_status = record
        elif record.frame.frame_type == FrameType.TIME_STATUS:
            if first_time is None:
                first_time = record
            last_time = record
        elif record.frame.frame_type == FrameType.L1S_FRAME:
            if first_l1s is None:
                first_l1s = record
            claim = parse_payload(record.frame)
            mt_first.setdefault(claim.message_type, record)
            pab_first.setdefault(claim.pab, record)
            sv_first.setdefault(claim.sv_id, record)
    if first_record is None:
        raise ValueError("real session contains no valid records")
    selected = []
    selected_offsets: set[int] = set()

    def add(record, reason):
        if record.offset not in selected_offsets:
            selected.append((record, reason))
            selected_offsets.add(record.offset)

    add(first_hello, "first HELLO")
    add(first_status, "first STATUS")
    add(first_time, "first TIME_STATUS")
    for message_type in (43, 44, 47, 48, 49, 50):
        add(mt_first[message_type], f"first MT{message_type}")
    for pab in (0x53, 0x9A, 0xC6):
        add(pab_first[pab], f"first PAB {pab:02X}")
    for sv_id in sorted(sv_first):
        add(sv_first[sv_id], f"first svId {sv_id}")
    add(last_time, "last TIME_STATUS")
    add(last_status, "last STATUS")
    add(last_record, "last archive record")
    selected.sort(key=lambda item: item[0].offset)

    session = validate_session(str(source))
    metadata = []
    for record, reason in selected:
        item = {"source_offset": record.offset, "pc_receive_ns": record.pc_receive_ns,
                "sequence": record.frame.sequence,
                "frame_type": FrameType(record.frame.frame_type).name,
                "reason": reason, "wire_sha256": sha256(record.wire).hexdigest()}
        if record.frame.frame_type == FrameType.L1S_FRAME:
            claim = parse_payload(record.frame)
            item.update({"mt": claim.message_type, "pab": f"{claim.pab:02X}",
                         "sv_id": claim.sv_id, "num_words": claim.num_words})
        metadata.append(item)

    source_hash = session["source"]["sha256"]
    manifest = {
        "schema": "hoshiguard.bridge_real_session_fixture.v1",
        "source": "real", "evidence_source": "real_hardware",
        "source_session": {"sha256": source_hash,
                           "size_bytes": source.stat().st_size,
                           "start_utc": session["start_utc"],
                           "end_utc": session["end_utc"],
                           "firmware": session["hello"]["first"]["firmware"],
                           "bridge_protocol": next(iter(session["protocol_versions"]))},
        "expected_sequence": session["sequence"],
        "expected_frame_counts": session["frame_type_distribution"],
        "expected_status_closure": session["status"],
        "expected_zero_drop_verdict": session["zero_l1s_drop_proven"],
        "selected_records": metadata,
        "privacy": {"coordinates_retained": False,
                    "nav_pvt_records_retained": False},
    }
    expected = {
        "source_sha256": source_hash,
        "records": session["hgraw_records"],
        "frame_type_distribution": session["frame_type_distribution"],
        "sequence": session["sequence"], "l1s": session["l1s"],
        "status_delta": session["status"]["delta"],
        "status_closed": session["status"]["closed"],
        "zero_l1s_drop_proven": session["zero_l1s_drop_proven"],
    }

    real_l1s = mt_first[43]
    frame = real_l1s.frame
    claim = parse_payload(frame)
    logical = bytearray(cobs_decode(real_l1s.wire[:-1])); logical[-1] ^= 1
    cobs_bad = bytearray(real_l1s.wire); cobs_bad[0] = 0
    gap_wire = replace_payload(frame, frame.payload,
                               sequence=(frame.sequence + 2) & 0xFFFFFFFF)

    ubx_payload = bytearray(frame.payload)
    ubx_start = 20
    ubx = bytearray(claim.ubx); ubx[-1] ^= 1
    ubx_payload[ubx_start:ubx_start + len(ubx)] = ubx
    ubx_bad = replace_payload(frame, bytes(ubx_payload))

    crc_payload = bytearray(frame.payload)
    ubx = bytearray(claim.ubx)
    ubx[15] ^= 0x80
    ubx[-2:] = bytes(ubx_checksum(ubx[2:-2]))
    crc_payload[ubx_start:ubx_start + len(ubx)] = ubx
    l1s_bad = replace_payload(frame, bytes(crc_payload))

    metadata_payload = bytearray(frame.payload); metadata_payload[4] ^= 0x01
    metadata_bad = replace_payload(frame, bytes(metadata_payload))

    first_record = archive_bytes([real_l1s])
    archive_bad = bytearray(first_record); archive_bad[-1] ^= 1
    negatives = {
        "schema": "hoshiguard.synthetic_derived_bridge_negatives.v1",
        "source": "synthetic_derived_from_real",
        "real_source_sha256": source_hash,
        "vectors": [
            {"name": "archive_crc_corrupted", "record_hex": archive_bad.hex().upper()},
            {"name": "cobs_corrupted", "wire_hex": bytes(cobs_bad).hex().upper()},
            {"name": "bridge_crc_corrupted",
             "wire_hex": (cobs_encode(bytes(logical)) + b"\0").hex().upper()},
            {"name": "sequence_gap", "wire_hex": gap_wire.hex().upper()},
            {"name": "ubx_checksum_corrupted", "wire_hex": ubx_bad.hex().upper()},
            {"name": "l1s_crc_corrupted", "wire_hex": l1s_bad.hex().upper()},
            {"name": "metadata_mismatch", "wire_hex": metadata_bad.hex().upper()},
        ],
    }

    artifacts = {
        "manifest.json": json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        "expected_summary.json": json.dumps(expected, indent=2, sort_keys=True) + "\n",
        "selected_records.hgraw": archive_bytes([item[0] for item in selected]),
        "negative_vectors.json": json.dumps(negatives, indent=2, sort_keys=True) + "\n",
    }
    return artifacts


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("session")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    artifacts = build(Path(args.session))
    if args.check:
        mismatched = [name for name, value in artifacts.items()
                      if not (DESTINATION / name).exists() or
                      (DESTINATION / name).read_bytes() !=
                      (value.encode("utf-8") if isinstance(value, str) else value)]
        if mismatched:
            print("bridge fixture mismatch: " + ", ".join(mismatched), file=sys.stderr)
            return 1
        print("bridge real-session fixture check OK")
        return 0
    DESTINATION.mkdir(parents=True, exist_ok=True)
    for name, value in artifacts.items():
        data = value.encode("utf-8") if isinstance(value, str) else value
        (DESTINATION / name).write_bytes(data)
    print(f"wrote {len(artifacts)} deterministic fixture artifacts to {DESTINATION}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
