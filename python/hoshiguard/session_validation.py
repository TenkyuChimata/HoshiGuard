"""Streaming validation of a real HoshiGuard Bridge archive."""

from __future__ import annotations

from collections import Counter
from dataclasses import asdict
from datetime import datetime, timezone
from hashlib import sha256
import json
from pathlib import Path
from typing import Any

from .bridge_frames import FrameType, Status, TimeStatus, parse_payload
from .bridge_protocol import SequenceDisposition, SequenceTracker
from .l1s import compare_claim
from .output.raw_archive import RawArchiveReader

_FRAME_NAMES = {int(value): value.name for value in FrameType}
_PPS_NAMES = {0: "NO", 1: "OBSERVED_ONCE", 2: "STABLE",
              3: "TRANSIENT_OR_MISSING", 4: "UNSTABLE"}
_TIME_NAMES = {0: "invalid", 1: "utc_valid", 2: "pps_only",
               3: "tentative", 4: "locked_utc_limited",
               5: "confirmed", 6: "degraded"}


def _iso(ns: int) -> str:
    return datetime.fromtimestamp(ns / 1_000_000_000,
                                  tz=timezone.utc).isoformat()


def _sha256_file(path: Path) -> str:
    digest = sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _status_dict(value: Status) -> dict[str, Any]:
    return asdict(value)


def _counter_delta(first: Status, last: Status) -> dict[str, int]:
    fields = (
        "valid_l1s", "bridge_generated", "bridge_queued", "bridge_sent",
        "bridge_dropped", "critical_dropped", "uart_output_bytes",
        "l1s_generated", "l1s_sent", "l1s_dropped", "encode_errors",
    )
    return {name: getattr(last, name) - getattr(first, name) for name in fields}


def _ndjson_summary(path: str | None) -> dict[str, Any] | None:
    if not path:
        return None
    counts: Counter[str] = Counter()
    invalid = 0
    objects = 0
    sequences: set[int] = set()
    with Path(path).open(encoding="utf-8") as stream:
        for line in stream:
            if not line.strip():
                continue
            try:
                item = json.loads(line)
            except (json.JSONDecodeError, UnicodeDecodeError):
                invalid += 1
                continue
            objects += 1
            counts[str(item.get("service", "unknown"))] += 1
            receipt = item.get("receipt", {})
            if isinstance(receipt.get("bridge_sequence"), int):
                sequences.add(receipt["bridge_sequence"])
    return {"path": str(Path(path)), "objects": objects,
            "invalid_lines": invalid, "service_distribution": dict(counts),
            "unique_bridge_sequences": len(sequences)}


def validate_session(path: str, ndjson: str | None = None) -> dict[str, Any]:
    source = Path(path)
    reader = RawArchiveReader(str(source))
    tracker = SequenceTracker()
    frame_types: Counter[str] = Counter()
    protocol_versions: Counter[str] = Counter()
    mt: Counter[str] = Counter()
    satellites: Counter[str] = Counter()
    pabs: Counter[str] = Counter()
    record_count = 0
    first_pc = 0
    last_pc = 0
    first_status_seen = False
    status_window_bridge = 0
    status_window_l1s = 0
    statuses: list[tuple[int, int, Status, int, int]] = []
    time_statuses: list[TimeStatus] = []
    hellos = []
    sequence_first: int | None = None
    sequence_last: int | None = None
    gaps = duplicates = out_of_order = rollover = 0
    l1s_count = ubx_errors = l1s_crc_errors = mismatches = 0
    l1s_structure_errors = 0
    second_bytes: Counter[int] = Counter()

    for index, record in enumerate(reader):
        frame = record.frame
        record_count += 1
        if record_count == 1:
            first_pc = record.pc_receive_ns
        last_pc = record.pc_receive_ns
        if first_status_seen:
            status_window_bridge += 1
            if frame.frame_type == FrameType.L1S_FRAME:
                status_window_l1s += 1
        frame_types[_FRAME_NAMES.get(frame.frame_type,
                                    f"UNKNOWN_{frame.frame_type}")] += 1
        protocol_versions[f"{frame.major}.{frame.minor}"] += 1
        second_bytes[record.pc_receive_ns // 1_000_000_000] += len(record.wire)
        if sequence_first is None:
            sequence_first = frame.sequence
        previous = tracker.last
        result = tracker.observe(frame.sequence,
                                 hello=frame.frame_type == FrameType.HELLO)
        if result.disposition == SequenceDisposition.GAP:
            gaps += result.missing
        elif result.disposition == SequenceDisposition.DUPLICATE:
            duplicates += 1
        elif result.disposition == SequenceDisposition.OUT_OF_ORDER:
            out_of_order += 1
        if previous == 0xFFFFFFFF and frame.sequence == 0:
            rollover += 1
        sequence_last = frame.sequence
        try:
            typed = parse_payload(frame)
        except ValueError:
            if frame.frame_type == FrameType.L1S_FRAME:
                l1s_structure_errors += 1
            continue
        if frame.frame_type == FrameType.HELLO:
            hellos.append(asdict(typed))
        elif frame.frame_type == FrameType.STATUS:
            statuses.append((index, frame.sequence, typed,
                             status_window_bridge, status_window_l1s))
            first_status_seen = True
        elif frame.frame_type == FrameType.TIME_STATUS:
            time_statuses.append(typed)
        elif frame.frame_type == FrameType.L1S_FRAME:
            l1s_count += 1
            comparison = compare_claim(typed)
            if comparison.verified is None:
                if any("UBX" in reason or "checksum" in reason
                       for reason in comparison.reasons):
                    ubx_errors += 1
                else:
                    l1s_structure_errors += 1
                continue
            verified = comparison.verified
            if not verified.valid_crc:
                l1s_crc_errors += 1
            if comparison.mismatch:
                mismatches += 1
            mt[str(verified.message_type)] += 1
            satellites[str(verified.sfrbx.sv_id)] += 1
            pabs[f"{verified.pab:02X}"] += 1

    duration = max(0.0, (last_pc - first_pc) / 1_000_000_000)
    status_closure: dict[str, Any] = {"available": len(statuses) >= 2}
    closure_ok = False
    if len(statuses) >= 2:
        first_index, first_seq, first_status, _, _ = statuses[0]
        last_index, last_seq, last_status, observed_bridge_window, \
            observed_l1s_window = statuses[-1]
        delta = _counter_delta(first_status, last_status)
        closure_checks = {
            "l1s_observed_equals_generated_delta":
                observed_l1s_window == delta["l1s_generated"],
            "l1s_observed_equals_sent_delta":
                observed_l1s_window == delta["l1s_sent"],
            "bridge_observed_equals_generated_delta":
                observed_bridge_window == delta["bridge_generated"],
            "bridge_observed_equals_sent_delta":
                observed_bridge_window == delta["bridge_sent"],
            "bridge_dropped_zero": delta["bridge_dropped"] == 0,
            "critical_dropped_zero": delta["critical_dropped"] == 0,
            "l1s_dropped_zero": delta["l1s_dropped"] == 0,
        }
        closure_ok = all(closure_checks.values())
        status_closure = {
            "available": True,
            "first_sequence": first_seq, "last_sequence": last_seq,
            "first": _status_dict(first_status),
            "last": _status_dict(last_status),
            "delta": delta,
            "l1s_frame_observed_in_status_window": observed_l1s_window,
            "bridge_frames_observed_in_status_window": observed_bridge_window,
            "checks": closure_checks, "closed": closure_ok,
        }

    archive_errors = asdict(reader.stats)
    error_free = all(archive_errors[name] == 0 for name in (
        "invalid_lengths", "truncated_records", "archive_crc_errors",
        "wire_length_errors", "cobs_errors", "bridge_crc_errors",
        "bridge_version_errors", "bridge_structure_errors"))
    sequence_ok = gaps == duplicates == out_of_order == 0
    verification_ok = (ubx_errors == l1s_crc_errors == mismatches ==
                       l1s_structure_errors == 0)
    zero_drop = closure_ok and sequence_ok and error_free and verification_ok
    average_bps = sum(second_bytes.values()) / duration if duration else 0.0
    peak_bps = max(second_bytes.values(), default=0)
    final_status = statuses[-1][2] if statuses else None
    report = {
        "schema": "hoshiguard.bridge_session_validation.v1",
        "evidence_source": "real_hardware",
        "source": {"path": str(source), "size_bytes": source.stat().st_size,
                   "sha256": _sha256_file(source)},
        "archive_version": f"{reader.archive_major}.{reader.archive_minor}",
        "start_utc": _iso(first_pc) if first_pc else None,
        "end_utc": _iso(last_pc) if last_pc else None,
        "duration_seconds": duration,
        "hgraw_records": record_count,
        "frame_type_distribution": dict(sorted(frame_types.items())),
        "protocol_versions": dict(sorted(protocol_versions.items())),
        "sequence": {"first": sequence_first, "last": sequence_last,
                     "epochs": tracker.epoch + (1 if record_count else 0),
                     "gaps": gaps, "duplicates": duplicates,
                     "out_of_order": out_of_order, "rollovers": rollover},
        "archive_validation": archive_errors,
        "l1s": {"frames": l1s_count, "ubx_checksum_errors": ubx_errors,
                "l1s_crc_errors": l1s_crc_errors,
                "structure_errors": l1s_structure_errors,
                "verification_mismatches": mismatches,
                "mt_distribution": dict(sorted(mt.items(), key=lambda x: int(x[0]))),
                "sv_id_distribution": dict(sorted(satellites.items(), key=lambda x: int(x[0]))),
                "pab_distribution": dict(sorted(pabs.items()))},
        "hello": {"count": len(hellos), "first": hellos[0] if hellos else None,
                  "last": hellos[-1] if hellos else None},
        "status": status_closure,
        "time_status": {"count": len(time_statuses),
                        "last": asdict(time_statuses[-1]) if time_statuses else None},
        "pps_final_state": _PPS_NAMES.get(final_status.pps_state, "UNKNOWN")
                           if final_status else "UNKNOWN",
        "time_quality_final": _TIME_NAMES.get(final_status.time_quality, "unknown")
                              if final_status else "unknown",
        "queue_high_watermark": max((item[2].queue_high_watermark
                                     for item in statuses), default=0),
        "wire_throughput": {"average_bytes_per_second": average_bps,
                            "peak_bytes_per_second": peak_bps,
                            "peak_115200_8n1_utilization": peak_bps / 11520.0},
        "ndjson": _ndjson_summary(ndjson),
        "warnings": [],
        "zero_l1s_drop_proven": zero_drop,
        "final_verdict": "PASS" if zero_drop else "FAIL",
    }
    if not closure_ok:
        report["warnings"].append("STATUS counter closure failed or unavailable")
    if not sequence_ok:
        report["warnings"].append("Bridge sequence discontinuity detected")
    if not error_free:
        report["warnings"].append("HGRAW/Bridge framing error detected")
    if not verification_ok:
        report["warnings"].append("UBX/L1S independent verification failed")
    return report


def markdown_report(report: dict[str, Any]) -> str:
    seq = report["sequence"]
    l1s = report["l1s"]
    archive = report["archive_validation"]
    closure = report["status"]
    return "\n".join((
        "# HoshiGuard v0.2.7 validation of the v0.2.5 real Bridge session",
        "",
        f"- Evidence: `{report['evidence_source']}`",
        f"- Source SHA-256: `{report['source']['sha256']}`",
        f"- Duration: {report['duration_seconds']:.3f} s",
        f"- HGRAW records: {report['hgraw_records']}",
        f"- Frame types: `{json.dumps(report['frame_type_distribution'], sort_keys=True)}`",
        f"- Sequence: {seq['first']}..{seq['last']}; gaps={seq['gaps']}; "
        f"duplicates={seq['duplicates']}; out_of_order={seq['out_of_order']}",
        f"- Archive CRC / COBS / Bridge CRC errors: "
        f"{archive['archive_crc_errors']} / {archive['cobs_errors']} / "
        f"{archive['bridge_crc_errors']}",
        f"- L1S: {l1s['frames']}; UBX errors={l1s['ubx_checksum_errors']}; "
        f"L1S CRC errors={l1s['l1s_crc_errors']}; mismatches={l1s['verification_mismatches']}",
        f"- STATUS closure: {closure.get('closed', False)}",
        f"- Queue high watermark: {report['queue_high_watermark']}",
        f"- PPS / time: {report['pps_final_state']} / {report['time_quality_final']}",
        f"- Peak wire utilization at 115200 8N1: "
        f"{report['wire_throughput']['peak_115200_8n1_utilization']:.2%}",
        f"- Zero L1S drop verdict: **{report['zero_l1s_drop_proven']}**",
        f"- Final verdict: **{report['final_verdict']}**",
        "",
        "Counters are deltas between the first and last STATUS in the archive; "
        "pre-capture cumulative counts are not compared with in-window frames.",
        "",
    ))
