#!/usr/bin/env python3
"""Audit an NDJSON file without trusting its filename or claimed mode."""

from __future__ import annotations

import argparse
from collections import Counter
from hashlib import sha256
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))
from hoshiguard.bridge_protocol import FrameType  # noqa: E402
from hoshiguard.output.raw_archive import RawArchiveReader  # noqa: E402


def digest(path: Path) -> str:
    value = sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def audit(ndjson: str, archive: str) -> dict:
    path = Path(ndjson)
    record_types = Counter()
    modes = Counter()
    services = Counter()
    mts = Counter()
    sequences: list[int] = []
    semantic = Counter()
    frames = Counter()
    events = Counter()
    invalid = duplicate_lines = 0
    line_hashes: set[str] = set()
    session_start = session_end = 0
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            if not line.strip():
                continue
            key = sha256(line.encode("utf-8")).hexdigest()
            duplicate_lines += int(key in line_hashes)
            line_hashes.add(key)
            try:
                item = json.loads(line)
            except json.JSONDecodeError:
                invalid += 1
                continue
            record_type = str(item.get("record_type", "missing"))
            record_types[record_type] += 1
            session_start += int(record_type == "session_start")
            session_end += int(record_type == "session_end")
            modes[str(item.get("stream_mode", "missing"))] += 1
            services[str(item.get("service", "missing"))] += 1
            receipt = item.get("receipt", {})
            if isinstance(receipt.get("bridge_sequence"), int):
                sequences.append(receipt["bridge_sequence"])
            if isinstance(receipt.get("mt"), int):
                mts[str(receipt["mt"])] += 1
            identity = item.get("identity", {})
            if identity.get("semantic_fingerprint"):
                semantic[identity["semantic_fingerprint"]] += 1
            if identity.get("frame_fingerprint"):
                frames[identity["frame_fingerprint"]] += 1
            if identity.get("event_key"):
                events[identity["event_key"]] += 1
    archive_l1s = {record.frame.sequence for record in RawArchiveReader(archive)
                   if record.frame.frame_type == FrameType.L1S_FRAME}
    output_sequences = set(sequences)
    explicit = [key for key in modes if key != "missing"]
    if len(explicit) == 1:
        actual_mode = explicit[0]
        confidence = "explicit_metadata"
    elif set(record_types) <= {"semantic_observation", "service_summary"}:
        actual_mode = "semantic_observations"
        confidence = "inferred_from_record_type"
    elif set(record_types) == {"frame_observation"}:
        actual_mode = "all_observations"
        confidence = "inferred_from_record_type"
    elif set(record_types) <= {"event_lifecycle", "service_summary"}:
        actual_mode = "events_only"
        confidence = "inferred_from_record_type"
    else:
        actual_mode = "unknown"
        confidence = "ambiguous"
    if confidence == "inferred_from_record_type" and actual_mode == "semantic_observations":
        finding = (
            "The filename cannot establish mode. Business records prove the "
            "then-default semantic mode suppressed repeated frames. Output "
            "sequences beyond the archive, if present, indicate different close "
            "boundaries rather than Bridge loss.")
    elif confidence == "explicit_metadata":
        finding = (f"Stream mode is explicitly declared as {actual_mode}; "
                   "accounting is evaluated according to that mode.")
    else:
        finding = "Stream mode cannot be established unambiguously."
    return {
        "schema": "hoshiguard.ndjson_stream_audit.v1",
        "source": {"path": str(path), "size_bytes": path.stat().st_size,
                   "sha256": digest(path)},
        "archive": {"path": archive, "l1s_frames": len(archive_l1s)},
        "objects": sum(record_types.values()), "invalid_json_lines": invalid,
        "duplicate_output_lines": duplicate_lines,
        "record_type_distribution": dict(record_types),
        "service_distribution": dict(services), "mt_distribution": dict(mts),
        "stream_mode_metadata": dict(modes), "actual_stream_mode": actual_mode,
        "mode_evidence": confidence,
        "session_start_records": session_start, "session_end_records": session_end,
        "sequence_first": min(sequences) if sequences else None,
        "sequence_last": max(sequences) if sequences else None,
        "unique_bridge_sequences": len(output_sequences),
        "unique_frame_fingerprints": len(frames),
        "unique_semantic_fingerprints": len(semantic),
        "unique_event_keys": len(events),
        "archive_sequence_overlap": len(output_sequences & archive_l1s),
        "archive_l1s_without_output": len(archive_l1s - output_sequences),
        "output_sequences_outside_archive": sorted(output_sequences - archive_l1s),
        "finding": finding,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ndjson")
    parser.add_argument("--archive", required=True)
    parser.add_argument("--json-report", default="reports/soak-24h-ndjson-audit.json")
    args = parser.parse_args()
    result = audit(args.ndjson, args.archive)
    target = Path(args.json_report)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    target.with_suffix(".md").write_text(
        "# Soak NDJSON mode audit\n\n"
        f"- Actual mode: `{result['actual_stream_mode']}` "
        f"({result['mode_evidence']})\n"
        f"- Objects: {result['objects']}\n"
        f"- Archive L1S: {result['archive']['l1s_frames']}\n"
        f"- Missing stream metadata: {result['stream_mode_metadata'].get('missing', 0)}\n"
        f"- Sequences outside archive: `{result['output_sequences_outside_archive']}`\n\n"
        f"{result['finding']}\n", encoding="utf-8")
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
