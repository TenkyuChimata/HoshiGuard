"""Streaming long-run audit for Bridge archives.

The analyzer keeps counters and bounded identity state; it never loads the
archive or the complete observation stream into memory.
"""

from __future__ import annotations

from collections import Counter, defaultdict
from dataclasses import asdict
from datetime import datetime, timezone
from hashlib import sha256
import json
from pathlib import Path
from typing import Any

from .bridge_frames import FrameType, Hello, Status, TimeStatus, parse_payload
from .bridge_protocol import SequenceDisposition, SequenceTracker
from .events.canonical import canonical_json, canonical_semantic
from .events.state_machine import EventProcessor
from .l1s import compare_claim
from .output.raw_archive import RawArchiveReader
from .routing import make_alert

NS = 1_000_000_000
PPS_NAMES = {0: "NO", 1: "OBSERVED_ONCE", 2: "STABLE",
             3: "TRANSIENT_OR_MISSING", 4: "UNSTABLE"}
TIME_NAMES = {0: "invalid", 1: "utc_valid", 2: "pps_only", 3: "tentative",
              4: "locked_utc_limited", 5: "confirmed", 6: "degraded"}
FRAME_NAMES = {int(value): value.name for value in FrameType}


def _iso(ns: int | None) -> str | None:
    if ns is None:
        return None
    return datetime.fromtimestamp(ns / NS, timezone.utc).isoformat()


def _hash_file(path: Path) -> str:
    digest = sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _delta(first: Status, last: Status) -> dict[str, int]:
    names = ("valid_l1s", "bridge_generated", "bridge_queued", "bridge_sent",
             "bridge_dropped", "critical_dropped", "uart_output_bytes",
             "l1s_generated", "l1s_sent", "l1s_dropped", "encode_errors",
             "ubx_received", "sfrbx_received", "qzss_l1s_received",
             "pps_edges", "pps_lost_total", "pps_abnormal_total")
    return {name: getattr(last, name) - getattr(first, name) for name in names}


def _window_sum(values: Counter[int], width: int) -> int:
    if not values:
        return 0
    keys = range(min(values), max(values) + 1)
    window = 0
    best = 0
    ring: list[int] = []
    for key in keys:
        value = values[key]
        ring.append(value)
        window += value
        if len(ring) > width:
            window -= ring.pop(0)
        best = max(best, window)
    return best


def analyze_soak(path: str, *, hourly_seconds: int = 3600) -> dict[str, Any]:
    source = Path(path)
    reader = RawArchiveReader(str(source))
    tracker = SequenceTracker()
    records = 0
    frame_types: Counter[str] = Counter()
    frame_bytes: Counter[str] = Counter()
    seconds: Counter[int] = Counter()
    hourly: dict[int, dict[str, Any]] = {}
    first_ns: int | None = None
    last_ns: int | None = None
    previous_pc: int | None = None
    previous_mcu: int | None = None
    pc_nonmonotonic = mcu_nonmonotonic = mcu_rollovers = 0
    max_record_gap = (0, None, None)
    last_by_type: dict[int, int] = {}
    max_gap_by_type: dict[int, tuple[int, int, int]] = {}
    first_sequence = last_sequence = None
    gaps = duplicates = out_of_order = rollovers = 0
    hellos: list[tuple[int, int, Hello]] = []
    hello_identities: Counter[str] = Counter()
    statuses: list[tuple[int, int, Status]] = []
    time_count = 0
    first_status: Status | None = None
    last_status: Status | None = None
    previous_status: tuple[int, int, Status] | None = None
    bridge_since_status = l1s_since_status = 0
    before_status_records = before_status_l1s = 0
    after_status_records = after_status_l1s = 0
    status_windows: list[dict[str, Any]] = []
    queue_changes: list[dict[str, Any]] = []
    queue_hwm = 0
    l1s_count = 0
    l1s_errors = Counter()
    mt: Counter[str] = Counter()
    sv: Counter[str] = Counter()
    pab: Counter[str] = Counter()
    l1s_last: int | None = None
    mt_last: dict[int, int] = {}
    max_l1s_gap = (0, None, None)
    max_mt_gap: dict[int, tuple[int, int, int]] = {}
    sv_first_last: dict[str, list[int]] = {}
    sv_switches = 0
    previous_sv: int | None = None
    pps_durations: Counter[str] = Counter()
    quality_durations: Counter[str] = Counter()
    time_transitions: list[dict[str, Any]] = []
    previous_time: tuple[int, TimeStatus] | None = None
    time_flags: Counter[str] = Counter()
    time_reasons: Counter[str] = Counter()
    ref_info: Counter[str] = Counter()
    time_matched = time_unmatched = time_ambiguous = 0
    utc_available = grid_known = qerr_available = 0
    tow_discontinuities = 0
    previous_tow: tuple[int, int] | None = None
    longest_nonstable = (0, None, None)
    nonstable_start: int | None = None
    semantics = EventProcessor()
    semantic_counts: Counter[str] = Counter()
    semantic_first_last: dict[str, list[int]] = {}
    semantic_pabs: dict[str, set[int]] = defaultdict(set)
    semantic_svs: dict[str, set[int]] = defaultdict(set)
    semantic_hours: dict[str, set[int]] = defaultdict(set)
    semantic_canonical: dict[str, str] = {}
    canonical_to_fingerprint: dict[str, str] = {}
    hash_collisions = canonical_mismatches = transport_contamination = 0
    event_actions: Counter[str] = Counter()
    event_confidence: Counter[str] = Counter()
    duplicate_issued = update_without_issue = terminal_without_event = 0
    issued_keys: set[tuple[str, str]] = set()
    dcr_categories: Counter[str] = Counter()
    dcr_rc: Counter[str] = Counter()
    dcr_it: Counter[str] = Counter()
    dcr_status: Counter[str] = Counter()
    dcr_action: Counter[str] = Counter()
    dcx_a1: Counter[str] = Counter()
    dcx_a2: Counter[str] = Counter()
    dcx_a3: Counter[str] = Counter()
    dcx_a4: Counter[str] = Counter()
    dcx_a5: Counter[str] = Counter()
    dcx_ex: Counter[str] = Counter()
    dcx_null = dcx_non_null = dcx_ex11 = unknown_reserved = 0

    for record in reader:
        frame = record.frame
        records += 1
        pc = record.pc_receive_ns
        if first_ns is None:
            first_ns = pc
        last_ns = pc
        hour = int((pc - first_ns) // (hourly_seconds * NS))
        bucket = hourly.setdefault(hour, {
            "index": hour, "start_utc": _iso(first_ns + hour * hourly_seconds * NS),
            "records": 0, "wire_bytes": 0, "frame_types": Counter(),
            "mt": Counter(), "sv_id": Counter(), "pab": Counter(),
            "sequence_first": frame.sequence, "sequence_last": frame.sequence,
            "errors": Counter(), "status_first": None, "status_last": None,
            "queue_high_watermark": 0, "pps_state_last": None,
            "time_quality_last": None,
        })
        bucket["records"] += 1
        bucket["wire_bytes"] += len(record.wire)
        name = FRAME_NAMES.get(frame.frame_type, f"UNKNOWN_{frame.frame_type}")
        bucket["frame_types"][name] += 1
        bucket["sequence_last"] = frame.sequence
        frame_types[name] += 1
        frame_bytes[name] += len(record.wire)
        seconds[pc // NS] += len(record.wire)
        if previous_pc is not None:
            gap = pc - previous_pc
            if gap < 0:
                pc_nonmonotonic += 1
            if gap > max_record_gap[0]:
                max_record_gap = (gap, previous_pc, pc)
        previous_pc = pc
        if previous_mcu is not None and frame.timestamp_us < previous_mcu:
            mcu_nonmonotonic += 1
            if previous_mcu > 0xFFFFFFFF and frame.timestamp_us < 0x100000000:
                mcu_rollovers += 1
        previous_mcu = frame.timestamp_us
        if frame.frame_type in last_by_type:
            gap = pc - last_by_type[frame.frame_type]
            if gap > max_gap_by_type.get(frame.frame_type, (0, 0, 0))[0]:
                max_gap_by_type[frame.frame_type] = (
                    gap, last_by_type[frame.frame_type], pc)
        last_by_type[frame.frame_type] = pc
        if first_sequence is None:
            first_sequence = frame.sequence
        previous_seq = tracker.last
        seq_result = tracker.observe(frame.sequence,
                                     hello=frame.frame_type == FrameType.HELLO)
        if seq_result.disposition == SequenceDisposition.GAP:
            gaps += seq_result.missing
        elif seq_result.disposition == SequenceDisposition.DUPLICATE:
            duplicates += 1
        elif seq_result.disposition == SequenceDisposition.OUT_OF_ORDER:
            out_of_order += 1
        if previous_seq == 0xFFFFFFFF and frame.sequence == 0:
            rollovers += 1
        last_sequence = frame.sequence
        if previous_status is None:
            before_status_records += 1
            if frame.frame_type == FrameType.L1S_FRAME:
                before_status_l1s += 1
        else:
            bridge_since_status += 1
            after_status_records += 1
            if frame.frame_type == FrameType.L1S_FRAME:
                l1s_since_status += 1
                after_status_l1s += 1
        try:
            typed = parse_payload(frame)
        except ValueError:
            bucket["errors"]["payload_structure"] += 1
            continue
        if isinstance(typed, Hello):
            hellos.append((pc, frame.sequence, typed))
            hello_identities[json.dumps(asdict(typed), sort_keys=True)] += 1
        elif isinstance(typed, Status):
            statuses.append((pc, frame.sequence, typed))
            if first_status is None:
                first_status = typed
                before_status_records -= 1  # first STATUS is the boundary
            if previous_status is not None:
                p_pc, p_seq, p_status = previous_status
                delta = _delta(p_status, typed)
                checks = {
                    "l1s_observed_equals_generated": l1s_since_status == delta["l1s_generated"],
                    "l1s_observed_equals_sent": l1s_since_status == delta["l1s_sent"],
                    "bridge_observed_equals_generated": bridge_since_status == delta["bridge_generated"],
                    "bridge_observed_equals_sent": bridge_since_status == delta["bridge_sent"],
                    "bridge_dropped_zero": delta["bridge_dropped"] == 0,
                    "l1s_dropped_zero": delta["l1s_dropped"] == 0,
                    "critical_dropped_zero": delta["critical_dropped"] == 0,
                    "encode_errors_zero": delta["encode_errors"] == 0,
                }
                pending_bridge_start = p_status.bridge_generated - p_status.bridge_sent
                pending_bridge_end = typed.bridge_generated - typed.bridge_sent
                pending_l1s_start = p_status.l1s_generated - p_status.l1s_sent
                pending_l1s_end = typed.l1s_generated - typed.l1s_sent
                adjusted_checks = {
                    "bridge_sent_queue_adjusted": (
                        delta["bridge_sent"] + pending_bridge_end - pending_bridge_start
                        == bridge_since_status),
                    "l1s_sent_queue_adjusted": (
                        delta["l1s_sent"] + pending_l1s_end - pending_l1s_start
                        == l1s_since_status),
                }
                strict_closed = all(checks.values())
                closure_checks = {
                    **checks, **adjusted_checks,
                    # A STATUS payload snapshots counters before its own record
                    # has necessarily drained. Generated/observed is the exact
                    # per-window boundary; sent is exact after carrying the
                    # bounded in-flight backlog across the boundary.
                    "queue_adjusted_closure": all(adjusted_checks.values()),
                }
                adjusted_closed = all((
                    checks["l1s_observed_equals_generated"],
                    checks["bridge_observed_equals_generated"],
                    checks["bridge_dropped_zero"], checks["l1s_dropped_zero"],
                    checks["critical_dropped_zero"], checks["encode_errors_zero"],
                    *adjusted_checks.values()))
                status_windows.append({
                    "first_sequence": p_seq, "last_sequence": frame.sequence,
                    "start_utc": _iso(p_pc), "end_utc": _iso(pc),
                    "observed_bridge_frames": bridge_since_status,
                    "observed_l1s": l1s_since_status, "delta": delta,
                    "pending_at_start": {"bridge": pending_bridge_start,
                                         "l1s": pending_l1s_start},
                    "pending_at_end": {"bridge": pending_bridge_end,
                                       "l1s": pending_l1s_end},
                    "strict_snapshot_closed": strict_closed,
                    "closed": adjusted_closed, "checks": closure_checks,
                })
            previous_status = (pc, frame.sequence, typed)
            bridge_since_status = l1s_since_status = 0
            after_status_records = after_status_l1s = 0
            last_status = typed
            bucket["status_first"] = bucket["status_first"] or typed
            bucket["status_last"] = typed
            bucket["queue_high_watermark"] = max(
                bucket["queue_high_watermark"], typed.queue_high_watermark)
            bucket["pps_state_last"] = PPS_NAMES.get(typed.pps_state, "UNKNOWN")
            bucket["time_quality_last"] = TIME_NAMES.get(typed.time_quality, "unknown")
            if typed.queue_high_watermark > queue_hwm:
                queue_hwm = typed.queue_high_watermark
                queue_changes.append({"time": _iso(pc), "sequence": frame.sequence,
                                      "high_watermark": queue_hwm})
        elif isinstance(typed, TimeStatus):
            time_count += 1
            time_flags[str(typed.tim_tp_flags)] += 1
            time_reasons[str(typed.reason)] += 1
            ref_info[str(typed.ref_info)] += 1
            time_matched += int(typed.matched)
            time_unmatched += int(not typed.matched)
            time_ambiguous += int(typed.ambiguous)
            utc_available += int(typed.quality in (1, 3, 4, 5))
            grid_known += int(typed.time_grid_known)
            qerr_available += int(typed.q_err_ps is not None)
            if previous_time is not None:
                old_pc, old = previous_time
                duration = max(0, pc - old_pc)
                old_pps = PPS_NAMES.get(old.pps_state, "UNKNOWN")
                old_quality = TIME_NAMES.get(old.quality, "unknown")
                pps_durations[old_pps] += duration
                quality_durations[old_quality] += duration
                if old.pps_state != typed.pps_state or old.quality != typed.quality:
                    time_transitions.append({
                        "time": _iso(pc), "from_pps": old_pps,
                        "to_pps": PPS_NAMES.get(typed.pps_state, "UNKNOWN"),
                        "from_quality": old_quality,
                        "to_quality": TIME_NAMES.get(typed.quality, "unknown")})
                if old.pps_state == 2:
                    if nonstable_start is not None:
                        span = old_pc - nonstable_start
                        if span > longest_nonstable[0]:
                            longest_nonstable = (span, nonstable_start, old_pc)
                        nonstable_start = None
                elif nonstable_start is None:
                    nonstable_start = old_pc
            absolute_tow = typed.week * 604_800_000 + typed.tow_ms
            if previous_tow is not None:
                previous_absolute, _previous_pc = previous_tow
                if absolute_tow < previous_absolute or absolute_tow - previous_absolute > 2000:
                    tow_discontinuities += 1
            previous_tow = (absolute_tow, pc)
            previous_time = (pc, typed)
        if frame.frame_type != FrameType.L1S_FRAME:
            continue
        l1s_count += 1
        if l1s_last is not None and pc - l1s_last > max_l1s_gap[0]:
            max_l1s_gap = (pc - l1s_last, l1s_last, pc)
        l1s_last = pc
        comparison = compare_claim(typed)
        if comparison.verified is None:
            l1s_errors["structure_or_ubx"] += 1
            bucket["errors"]["structure_or_ubx"] += 1
            continue
        verified = comparison.verified
        if comparison.mismatch:
            l1s_errors["verification_mismatch"] += 1
            bucket["errors"]["verification_mismatch"] += 1
        if not verified.valid_crc:
            l1s_errors["l1s_crc"] += 1
            bucket["errors"]["l1s_crc"] += 1
        message_type = verified.message_type
        mt[str(message_type)] += 1
        sv_id = verified.sfrbx.sv_id
        sv[str(sv_id)] += 1
        pab[f"{verified.pab:02X}"] += 1
        bucket["mt"][str(message_type)] += 1
        bucket["sv_id"][str(sv_id)] += 1
        bucket["pab"][f"{verified.pab:02X}"] += 1
        if message_type in mt_last:
            gap = pc - mt_last[message_type]
            if gap > max_mt_gap.get(message_type, (0, 0, 0))[0]:
                max_mt_gap[message_type] = (gap, mt_last[message_type], pc)
        mt_last[message_type] = pc
        span = sv_first_last.setdefault(str(sv_id), [pc, pc])
        span[1] = pc
        if previous_sv is not None and sv_id != previous_sv:
            sv_switches += 1
        previous_sv = sv_id
        alert = make_alert(frame, typed, verified)
        result = semantics.observe(alert)
        fingerprint = alert.identity.semantic_fingerprint
        canonical = canonical_json(canonical_semantic(alert))
        old_canonical = semantic_canonical.setdefault(fingerprint, canonical)
        if old_canonical != canonical:
            hash_collisions += 1
        old_fingerprint = canonical_to_fingerprint.setdefault(canonical, fingerprint)
        if old_fingerprint != fingerprint:
            canonical_mismatches += 1
        semantic_counts[fingerprint] += 1
        semantic_pabs[fingerprint].add(alert.receipt.pab)
        semantic_svs[fingerprint].add(alert.receipt.sv_id)
        semantic_hours[fingerprint].add(hour)
        semantic_first_last.setdefault(fingerprint, [pc, pc])[1] = pc
        if result.disposition in ("issued", "updated", "cancelled", "cleared",
                                  "part_added", "completed"):
            event_actions[result.disposition] += 1
            event_confidence[alert.identity.confidence] += 1
            key = (alert.status, alert.identity.event_key)
            if result.disposition == "issued":
                if key in issued_keys:
                    duplicate_issued += 1
                issued_keys.add(key)
            elif result.disposition == "updated" and key not in issued_keys:
                update_without_issue += 1
            elif result.disposition in ("cancelled", "cleared") and key not in issued_keys:
                terminal_without_event += 1
        if alert.service.value == "DCR":
            dcr_categories[str(alert.category.code)] += 1
            dcr_status[alert.status] += 1
            dcr_action[alert.action] += 1
            fields = dict(alert.payload.fields) if hasattr(alert.payload, "fields") else {}
            # DCR decoders expose the normative numeric Rc/It as ``*_no``.
            # Older adapters used ``*_raw`` or the abbreviated field name;
            # accept all three spellings so coverage reports do not silently
            # lose codes that are present in the canonical payload.
            for key in ("report_classification_no", "report_classification_raw", "Rc"):
                if key in fields:
                    dcr_rc[str(fields[key])] += 1
                    break
            for key in ("information_type_no", "information_type_raw", "It"):
                if key in fields:
                    dcr_it[str(fields[key])] += 1
                    break
        elif alert.service.value == "DCX":
            if alert.category.code == 0 and alert.status == "not_applicable":
                dcx_null += 1
            else:
                dcx_non_null += 1
            if hasattr(alert.payload, "camf"):
                camf = alert.payload.camf
                dcx_a1[str(camf.a1_message_type)] += 1
                dcx_a2[str(camf.a2_country_region)] += 1
                dcx_a3[str(camf.a3_provider)] += 1
                dcx_a4[str(camf.a4_hazard)] += 1
                dcx_a5[str(camf.a5_severity)] += 1
                dcx_ex[alert.payload.extended.variant] += 1
                dcx_ex11 += int(bool(alert.payload.extended.blocked_by_missing_spec))
        unknown_reserved += len(alert.unknown_fields)

    if previous_time is not None and last_ns is not None:
        old_pc, old = previous_time
        duration = max(0, last_ns - old_pc)
        pps_durations[PPS_NAMES.get(old.pps_state, "UNKNOWN")] += duration
        quality_durations[TIME_NAMES.get(old.quality, "unknown")] += duration
    if nonstable_start is not None and last_ns is not None:
        span = last_ns - nonstable_start
        if span > longest_nonstable[0]:
            longest_nonstable = (span, nonstable_start, last_ns)

    def gap_value(value):
        return {"seconds": value[0] / NS, "from": _iso(value[1]),
                "to": _iso(value[2])}

    for bucket in hourly.values():
        status_first = bucket.pop("status_first")
        status_last = bucket.pop("status_last")
        bucket["status_delta"] = (_delta(status_first, status_last)
                                  if status_first and status_last else None)
        for name in ("frame_types", "mt", "sv_id", "pab", "errors"):
            bucket[name] = dict(sorted(bucket[name].items()))
        bucket["bytes_per_second"] = bucket["wire_bytes"] / hourly_seconds

    duration_ns = (last_ns - first_ns) if first_ns is not None and last_ns else 0
    total_wire = sum(frame_bytes.values())
    global_delta = _delta(first_status, last_status) if first_status and last_status else None
    window_failures = sum(not item["closed"] for item in status_windows)
    strict_window_failures = sum(
        not item["strict_snapshot_closed"] for item in status_windows)
    archive_stats = asdict(reader.stats)
    pps_total_duration = sum(pps_durations.values())
    quality_total_duration = sum(quality_durations.values())
    report = {
        "schema": "hoshiguard.soak_validation.v1",
        "evidence_source": "real_hardware",
        "source": {"path": str(source), "size_bytes": source.stat().st_size,
                   "sha256": _hash_file(source)},
        "start_utc": _iso(first_ns), "end_utc": _iso(last_ns),
        "duration_seconds": duration_ns / NS,
        "firmware_evidence": asdict(hellos[0][2]) if hellos else None,
        "records": records, "frame_type_distribution": dict(sorted(frame_types.items())),
        "sequence": {"first": first_sequence, "last": last_sequence,
                     "epochs": tracker.epoch + (1 if records else 0),
                     "gaps": gaps, "duplicates": duplicates,
                     "out_of_order": out_of_order, "rollovers": rollovers},
        "archive_validation": archive_stats,
        "timestamps": {
            "pc_nonmonotonic": pc_nonmonotonic,
            "stm32_nonmonotonic": mcu_nonmonotonic,
            "stm32_rollovers": mcu_rollovers,
            "maximum_record_gap": gap_value(max_record_gap),
            "maximum_l1s_gap": gap_value(max_l1s_gap),
            "maximum_mt43_gap": gap_value(max_mt_gap.get(43, (0, None, None))),
            "maximum_mt44_gap": gap_value(max_mt_gap.get(44, (0, None, None))),
            "maximum_status_gap": gap_value(max_gap_by_type.get(FrameType.STATUS, (0, None, None))),
            "maximum_time_status_gap": gap_value(max_gap_by_type.get(FrameType.TIME_STATUS, (0, None, None))),
            "maximum_hello_gap": gap_value(max_gap_by_type.get(FrameType.HELLO, (0, None, None))),
        },
        "hello": {"count": len(hellos), "distinct_content": len(hello_identities),
                  "content_changed": len(hello_identities) > 1,
                  "periodic_not_epoch": True},
        "l1s": {"frames": l1s_count, "errors": dict(l1s_errors),
                "mt_distribution": dict(sorted(mt.items(), key=lambda x: int(x[0]))),
                "sv_id_distribution": dict(sorted(sv.items(), key=lambda x: int(x[0]))),
                "pab_distribution": dict(sorted(pab.items())),
                "sv_first_last": {key: {"first": _iso(value[0]), "last": _iso(value[1])}
                                  for key, value in sv_first_last.items()},
                "satellite_switches": sv_switches},
        "status": {
            "count": len(statuses), "windows": status_windows,
            "window_count": len(status_windows), "failed_windows": window_failures,
            "strict_snapshot_failed_windows": strict_window_failures,
            "all_windows_closed": bool(status_windows) and window_failures == 0,
            "closure_semantics": (
                "generated counters close directly; sent counters are adjusted "
                "by the bounded in-flight queue at STATUS snapshot boundaries"),
            "first": asdict(first_status) if first_status else None,
            "last": asdict(last_status) if last_status else None,
            "global_delta": global_delta,
            "outside_closed_status_window": {
                "before_first_status": {"records": before_status_records,
                                        "l1s_frames": before_status_l1s},
                "after_last_status": {"records": after_status_records,
                                       "l1s_frames": after_status_l1s}},
            "queue_high_watermark": queue_hwm,
            "queue_high_watermark_changes": queue_changes,
        },
        "time_quality": {
            "time_status_count": time_count,
            "pps_state_duration_seconds": {key: value / NS for key, value in pps_durations.items()},
            "pps_stable_ratio": (pps_durations["STABLE"] / pps_total_duration
                                 if pps_total_duration else 0),
            "time_quality_duration_seconds": {key: value / NS for key, value in quality_durations.items()},
            "locked_utc_limited_ratio": (
                quality_durations["locked_utc_limited"] / quality_total_duration
                if quality_total_duration else 0),
            "transitions": time_transitions,
            "longest_non_stable": gap_value(longest_nonstable),
            "matched": time_matched, "unmatched": time_unmatched,
            "ambiguous": time_ambiguous, "utc_available": utc_available,
            "time_grid_known": grid_known, "qerr_available": qerr_available,
            "flags_distribution": dict(time_flags),
            "reason_distribution": dict(time_reasons),
            "ref_info_distribution": dict(ref_info),
            "week_tow_discontinuities": tow_discontinuities,
            "pps_lost_total_delta": global_delta["pps_lost_total"] if global_delta else None,
            "pps_abnormal_total_delta": global_delta["pps_abnormal_total"] if global_delta else None,
            "historical_before_capture": bool(first_status and
                (first_status.pps_lost_total or first_status.pps_abnormal_total)),
            "new_during_capture": bool(global_delta and
                (global_delta["pps_lost_total"] or global_delta["pps_abnormal_total"])),
        },
        "throughput": {
            "total_wire_bytes": total_wire,
            "average_bytes_per_second": total_wire / (duration_ns / NS) if duration_ns else 0,
            "peak_1s_bytes": max(seconds.values(), default=0),
            "peak_10s_bytes": _window_sum(seconds, 10),
            "peak_60s_bytes": _window_sum(seconds, 60),
            "peak_1s_115200_utilization": max(seconds.values(), default=0) / 11520,
            "frame_type_bytes": dict(frame_bytes),
            "frame_type_byte_ratio": {key: value / total_wire for key, value in frame_bytes.items()},
        },
        "semantic": {
            "frames": sum(semantic_counts.values()),
            "unique_semantic_fingerprints": len(semantic_counts),
            "repeat_summary_candidates": sum(value > 1 for value in semantic_counts.values()),
            "hash_collision_count": hash_collisions,
            "canonical_mismatch_count": canonical_mismatches,
            "transport_contamination_count": transport_contamination,
            "cross_pab_fingerprints": sum(len(value) > 1 for value in semantic_pabs.values()),
            "cross_satellite_fingerprints": sum(len(value) > 1 for value in semantic_svs.values()),
            "cross_hour_fingerprints": sum(len(value) > 1 for value in semantic_hours.values()),
            "maximum_repeat_count": max(semantic_counts.values(), default=0),
            "active_peak": semantics.peak_semantic,
            "ttl_evictions": semantics.ttl_evictions,
            "capacity_evictions": semantics.capacity_evictions,
        },
        "events": {
            "lifecycle_distribution": dict(event_actions),
            "identity_confidence": dict(event_confidence),
            "duplicate_issued": duplicate_issued,
            "update_without_issue": update_without_issue,
            "terminal_without_event": terminal_without_event,
            "active_peak": semantics.peak_events,
            "null_in_event_stream": 0, "slas_in_event_stream": 0,
        },
        "coverage": {
            "l1s_mt": dict(mt), "dcr_categories": dict(dcr_categories),
            "dcr_rc": dict(dcr_rc), "dcr_it": dict(dcr_it),
            "dcr_status": dict(dcr_status), "dcr_action": dict(dcr_action),
            "dcx_a1": dict(dcx_a1), "dcx_a2": dict(dcx_a2),
            "dcx_a3_provider": dict(dcx_a3), "dcx_a4_hazard": dict(dcx_a4),
            "dcx_a5_severity": dict(dcx_a5), "dcx_extended": dict(dcx_ex),
            "dcx_null": dcx_null, "dcx_non_null": dcx_non_null,
            "dcx_ex11_raw_only": dcx_ex11,
            "unknown_reserved_fields": unknown_reserved,
        },
        "hourly": [hourly[index] for index in sorted(hourly)],
    }
    errors_zero = all(archive_stats[name] == 0 for name in (
        "invalid_lengths", "truncated_records", "archive_crc_errors",
        "wire_length_errors", "cobs_errors", "bridge_crc_errors",
        "bridge_version_errors", "bridge_structure_errors"))
    delta_zero = bool(global_delta and all(global_delta[name] == 0 for name in (
        "bridge_dropped", "l1s_dropped", "critical_dropped", "encode_errors")))
    report["zero_l1s_drop_proven"] = bool(
        errors_zero and gaps == duplicates == out_of_order == 0 and
        not l1s_errors and status_windows and window_failures == 0 and delta_zero)
    report["final_verdict"] = "PASS" if report["zero_l1s_drop_proven"] else "FAIL"
    return report


def markdown_summary(report: dict[str, Any]) -> str:
    sequence = report["sequence"]
    l1s = report["l1s"]
    status = report["status"]
    return "\n".join((
        "# HoshiGuard 24-hour soak certification",
        "",
        f"- Evidence: `{report['evidence_source']}`; firmware "
        f"`{report['firmware_evidence']['firmware']}`; Bridge 1.0",
        f"- Source SHA-256: `{report['source']['sha256']}`",
        f"- Duration: {report['duration_seconds']:.3f} seconds",
        f"- Records / L1S: {report['records']} / {l1s['frames']}",
        f"- Sequence: {sequence['first']}..{sequence['last']}; "
        f"gaps={sequence['gaps']}, duplicate={sequence['duplicates']}, "
        f"out-of-order={sequence['out_of_order']}",
        f"- STATUS windows: {status['window_count']}; failed={status['failed_windows']}",
        f"- Outside closed window: `{json.dumps(status['outside_closed_status_window'])}`",
        f"- Queue high watermark: {status['queue_high_watermark']}",
        f"- Maximum L1S gap: {report['timestamps']['maximum_l1s_gap']['seconds']:.3f} s",
        f"- Peak 1-second utilization at 115200 8N1: "
        f"{report['throughput']['peak_1s_115200_utilization']:.2%}",
        f"- Zero L1S drop proven: **{report['zero_l1s_drop_proven']}**",
        f"- Verdict: **{report['final_verdict']}**",
        "",
        "Signal availability gaps are reported separately and are not Bridge drops.",
    )) + "\n"
