#!/usr/bin/env python3
"""完全な UBX packet とテキストの集計を分けて捕捉データを分析する。"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import statistics
import sys
from collections import Counter, defaultdict
from typing import Iterable, Optional

import capture_io
import replay_ubx

GNSS_NAMES = {
    0: "GPS", 1: "SBAS", 2: "Galileo", 3: "BeiDou", 4: "IMES",
    5: "QZSS", 6: "GLONASS", 7: "NavIC",
}
KV_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)")
TIMESTAMP_RE = re.compile(r"\btimestamp_us=(\d+)")


def key_values(line: str) -> dict[str, str]:
    return dict(KV_RE.findall(line))


def integer(value: Optional[str], default: int = 0) -> int:
    try:
        return int(value or "", 0)
    except ValueError:
        return default


def _counter_json(counter: Counter[int]) -> dict[str, int]:
    return {str(key): value for key, value in sorted(counter.items())}


def analyze_lines(lines: Iterable[str], input_info: Optional[capture_io.CaptureInput] = None) -> dict[str, object]:
    material = list(lines)
    records, errors = replay_ubx.parse_log(material)
    raw_sfrbx: list[replay_ubx.SfrbxRecord] = []
    text_headers: list[dict[str, int]] = []
    pps: list[dict[str, int]] = []
    matches: Counter[str] = Counter()
    match_reasons: Counter[str] = Counter()
    tim_flags: Counter[str] = Counter()
    qerr_available: Counter[str] = Counter()
    limited_timtp_evidence = 0
    status_records: list[dict[str, str]] = []
    all_timestamps: list[int] = []

    for record in records:
        header = replay_ubx.parse_sfrbx(record.packet)
        if header is not None:
            raw_sfrbx.append(header)

    for line in material:
        values = key_values(line)
        timestamp = TIMESTAMP_RE.search(line)
        if timestamp:
            all_timestamps.append(int(timestamp.group(1)))
        if line.startswith("SFRBX_HDR "):
            text_headers.append({key: integer(values.get(key)) for key in
                                 ("gnssId", "svId", "sigId", "freqId", "numWords")})
        elif line.startswith("PPS timestamp_us="):
            required = ("timestamp_us", "seq", "interval_us")
            if all(key in values for key in required):
                event = {key: integer(values.get(key)) for key in
                         ("timestamp_us", "seq", "interval_us", "lost",
                          "abnormal")}
                event["lost_total"] = integer(
                    values.get("pps_lost_total"), event["lost"])
                event["abnormal_total"] = integer(
                    values.get("pps_abnormal_total"), event["abnormal"])
                event["consecutive_good"] = integer(
                    values.get("pps_consecutive_good"))
                pps.append(event)
        elif line.startswith("TIMTP "):
            tim_flags[values.get("flags", "unknown")] += 1
            qerr_available[values.get("qerr_valid", "unknown")] += 1
            if (values.get("time_base") == "UTC" and
                    values.get("utc_available") == "yes" and
                    values.get("locked") == "yes" and
                    values.get("qerr_valid") == "no"):
                limited_timtp_evidence += 1
        elif line.startswith("TIMTP_MATCH "):
            matches[values.get("quality", "unknown")] += 1
            match_reasons[values.get("reason", "unknown")] += 1
        elif line.startswith("STATUS "):
            status_records.append(values)

    canonical_gnss = Counter(header.gnss_id for header in raw_sfrbx)
    text_gnss = Counter(item["gnssId"] for item in text_headers)
    qzss = [header for header in raw_sfrbx if header.gnss_id == 5]
    by_sv = Counter(header.sv_id for header in qzss)
    by_sig = Counter(header.sig_id for header in qzss)
    word_counts = Counter(header.num_words for header in qzss)

    seen: dict[bytes, int] = {}
    l1s_records: list[replay_ubx.L1sRecord] = []
    senders: dict[bytes, set[int]] = defaultdict(set)
    for record in records:
        l1s = replay_ubx.extract_l1s(record, seen)
        if l1s is None:
            continue
        l1s_records.append(l1s)
        canonical = l1s.raw[:31] + bytes((l1s.raw[31] & 0xC0,))
        senders[canonical].add(l1s.sv_id)

    pps_intervals = [event["interval_us"] for event in pps
                     if event["interval_us"] > 0]
    wrap_count = sum(1 for previous, current in zip(pps, pps[1:])
                     if current["timestamp_us"] < previous["timestamp_us"])
    pps_low = [event["timestamp_us"] & 0xFFFFFFFF for event in pps]
    nav_delays: list[int] = []
    for record in records:
        if record.packet[2:4] != b"\x01\x07":
            continue
        current = record.timestamp_us & 0xFFFFFFFF
        candidates = [(current - edge) & 0xFFFFFFFF for edge in pps_low]
        candidates = [delay for delay in candidates if delay <= 1_100_000]
        if candidates:
            nav_delays.append(min(candidates))

    start = min(all_timestamps) if all_timestamps else None
    end = max(all_timestamps) if all_timestamps else None
    raw_start = min((record.timestamp_us for record in records), default=None)
    raw_end = max((record.timestamp_us for record in records), default=None)
    nav_count = sum(record.packet[2:4] == b"\x01\x07" for record in records)
    timtp_count = sum(record.packet[2:4] == b"\x0D\x01" for record in records)
    checksum_errors = sum("checksum mismatch" in error for error in errors)
    malformed = len(errors) - checksum_errors
    last_status = status_records[-1] if status_records else {}
    firmware_l1s = integer(last_status.get("l1s"))
    saved_l1s = len(l1s_records)
    missing_l1s = max(0, firmware_l1s - saved_l1s)
    log_dropped = integer(last_status.get("log_dropped"),
                          integer(last_status.get("drops")))
    log_queued = integer(last_status.get("log_queued"))
    attempted_logs = log_queued + log_dropped
    has_nav_raw = nav_count != 0
    legacy_limited_matches = min(
        limited_timtp_evidence,
        match_reasons["qerr_invalid"] + match_reasons["qerr_unavailable"])
    limited_matches = matches["locked_utc_limited"] + legacy_limited_matches

    file_report: dict[str, object] = {
        "size_bytes": input_info.size_bytes if input_info else None,
        "sha256": input_info.sha256 if input_info else None,
        "first_valid_record_offset": input_info.first_record_offset if input_info else 0,
        "leading_garbage_bytes": input_info.leading_garbage_bytes if input_info else 0,
        "leading_summary_hex": input_info.leading_summary_hex if input_info else "",
        "start_timestamp_us": start,
        "end_timestamp_us": end,
        "duration_seconds": None if start is None else (end - start) / 1_000_000,
        "ubx_duration_seconds": None if raw_start is None else (raw_end - raw_start) / 1_000_000,
        "ubx_raw_records": len(records) + len(errors),
        "ubx_raw": len(records) + len(errors),
        "ubx_checksum_ok": len(records),
        "ubx_checksum_error": checksum_errors,
        "ubx_malformed": malformed,
        "rxm_sfrbx_full_ubx": len(raw_sfrbx),
        "rxm_sfrbx": len(raw_sfrbx),
        "sfrbx_header_records": len(text_headers),
        "nav_pvt_full_ubx": nav_count,
        "nav_pvt": nav_count,
        "tim_tp_full_ubx": timtp_count,
        "tim_tp": timtp_count,
        "pps_records": len(pps),
        "pps": len(pps),
        "log_dropped": log_dropped,
        "log_dropped_last_reported": log_dropped,
        "log_drop_ratio_estimate": (log_dropped / attempted_logs
                                    if attempted_logs else None),
    }
    mt_counts = Counter(item.message_type for item in l1s_records)
    pab_counts = Counter(item.pab for item in l1s_records)
    report: dict[str, object] = {
        "file": file_report,
        "canonical_full_ubx_gnss": {
            GNSS_NAMES.get(key, f"unknown_{key}"): value
            for key, value in sorted(canonical_gnss.items())
        },
        "saved_sfrbx_header_gnss": {
            GNSS_NAMES.get(key, f"unknown_{key}"): value
            for key, value in sorted(text_gnss.items())
        },
        # 互換性のための key。現在は完全な UBX packet のみを意味する。
        "gnss": {GNSS_NAMES.get(key, f"unknown_{key}"): value
                 for key, value in sorted(canonical_gnss.items())},
        "qzss": {
            "by_sv_id": _counter_json(by_sv),
            "by_sig_id": _counter_json(by_sig),
            "l1ca": by_sig[0],
            "l1s": by_sig[1],
            "num_words": _counter_json(word_counts),
            "pab": {f"{key:02X}": value for key, value in sorted(pab_counts.items())},
            "valid_pab": sum(item.valid_pab for item in l1s_records),
            "invalid_pab": sum(not item.valid_pab for item in l1s_records),
            "valid_crc": sum(item.valid_crc for item in l1s_records),
            "invalid_crc": sum(not item.valid_crc for item in l1s_records),
            "mt": _counter_json(mt_counts),
            "mt43": mt_counts[43],
            "mt44": mt_counts[44],
            "other_mt": sum(value for key, value in mt_counts.items()
                            if key not in (43, 44)),
            "duplicates": sum(item.duplicate for item in l1s_records),
            "duplicate_satellites": {
                raw.hex().upper(): sorted(satellites)
                for raw, satellites in senders.items() if len(satellites) > 1
            },
        },
        "firmware_vs_saved": {
            "firmware_l1s_last_reported": firmware_l1s,
            "saved_complete_l1s_ubx": saved_l1s,
            "minimum_missing_l1s_records": missing_l1s,
            "minimum_missing_l1s_ratio": (missing_l1s / firmware_l1s
                                           if firmware_l1s else None),
            "firmware_sfrbx_last_reported": integer(last_status.get("sfrbx")),
            "saved_complete_sfrbx_ubx": len(raw_sfrbx),
            "firmware_qzss_last_reported": integer(last_status.get("qzss_sfrbx")),
            "saved_complete_qzss_ubx": len(qzss),
            "firmware_l1ca_last_reported": integer(last_status.get("l1ca")),
            "saved_complete_l1ca_ubx": by_sig[0],
        },
        "time": {
            "pps_interval_average_us": statistics.mean(pps_intervals) if pps_intervals else None,
            "pps_interval_min_us": min(pps_intervals) if pps_intervals else None,
            "pps_interval_max_us": max(pps_intervals) if pps_intervals else None,
            "pps_lost_total_last": max((event.get("lost_total", 0) or event.get("lost", 0)
                                         for event in pps), default=0),
            "pps_abnormal_total_last": max((event.get("abnormal_total", 0) or event.get("abnormal", 0)
                                             for event in pps), default=0),
            "micros_wraps": wrap_count,
            "nav_to_previous_pps_samples": len(nav_delays),
            "nav_to_previous_pps_min_us": min(nav_delays) if nav_delays else None,
            "nav_to_previous_pps_max_us": max(nav_delays) if nav_delays else None,
            "nav_to_previous_pps_average_us": statistics.mean(nav_delays) if nav_delays else None,
            "tim_tp_flags": dict(tim_flags),
            "tim_tp_qerr_valid": dict(qerr_available),
            "match_quality": dict(matches),
            "match_reason": dict(match_reasons),
            "tentative": matches["tentative"],
            "locked_utc_qerr_unavailable_evidence": limited_timtp_evidence,
            "locked_utc_limited": limited_matches,
            "confirmed": matches["confirmed"],
        },
        "privacy": {
            "nav_pvt_raw_present": has_nav_raw,
            "privacy_warning": ("precise coordinates may be recoverable"
                                if has_nav_raw else "none"),
        },
        "warnings": list(input_info.warnings) if input_info else [],
        "errors": errors,
    }
    return report


def print_human(report: dict[str, object]) -> None:
    file_report = report["file"]
    qzss = report["qzss"]
    timing = report["time"]
    print("CAPTURE " + " ".join(f"{key}={value}" for key, value in file_report.items()))
    print("GNSS_FULL_UBX " + " ".join(
        f"{key}={value}" for key, value in report["canonical_full_ubx_gnss"].items()))
    print("GNSS_SAVED_HEADERS " + " ".join(
        f"{key}={value}" for key, value in report["saved_sfrbx_header_gnss"].items()))
    print("QZSS " + " ".join(f"{key}={value}" for key, value in qzss.items()
                              if key not in ("by_sv_id", "by_sig_id", "num_words",
                                             "duplicate_satellites", "pab", "mt")))
    print(f"QZSS_SV {qzss['by_sv_id']} QZSS_SIG {qzss['by_sig_id']} "
          f"words={qzss['num_words']} pab={qzss['pab']} mt={qzss['mt']}")
    print("TIME " + " ".join(f"{key}={value}" for key, value in timing.items()
                              if key not in ("match_quality", "match_reason", "tim_tp_flags",
                                             "tim_tp_qerr_valid")))
    print("FIRMWARE_VS_SAVED " + " ".join(
        f"{key}={value}" for key, value in report["firmware_vs_saved"].items()))
    print(f"PRIVACY privacy_warning={report['privacy']['privacy_warning']}")
    for warning in report["warnings"]:
        print(f"WARNING {warning}")
    for error in report["errors"]:
        print(f"WARNING {error}")


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("--json", type=pathlib.Path, dest="json_path")
    args = parser.parse_args(argv)
    try:
        capture = capture_io.read_capture(args.capture)
        report = analyze_lines(capture.lines, capture)
    except OSError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print_human(report)
    if args.json_path is not None:
        try:
            with args.json_path.open("w", encoding="utf-8", newline="\n") as output:
                json.dump(report, output, indent=2, sort_keys=True)
                output.write("\n")
        except OSError as exc:
            print(f"error: cannot write JSON report: {exc}", file=sys.stderr)
            return 2
    return 1 if report["errors"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
