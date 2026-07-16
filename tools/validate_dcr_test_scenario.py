#!/usr/bin/env python3
"""Validate received DCR observations against the July 2026 official oracle.

The PDF manifest supplies expected scenarios only.  All bit decoding still
comes from IS-QZSS-DCR-016 through HoshiGuard's normal verified pipeline.
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from datetime import datetime
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

from hoshiguard.bridge_frames import FrameType, parse_payload
from hoshiguard.dcr.reassembly import NankaiTroughReassembler
from hoshiguard.l1s import compare_claim
from hoshiguard.models import json_safe
from hoshiguard.output.raw_archive import RawArchiveReader
from hoshiguard.routing import make_alert


def _fields(item):
    payload = item.get("payload", {})
    values = payload.get("fields", []) if isinstance(payload, dict) else []
    return dict(values)


def _observations(path: Path, input_format: str):
    decode_failures = crc_failures = 0
    values = []
    if input_format == "ndjson":
        with path.open(encoding="utf-8") as stream:
            for line in stream:
                try:
                    item = json.loads(line)
                except json.JSONDecodeError:
                    decode_failures += 1
                    continue
                if item.get("record_type") == "service_summary":
                    continue
                if item.get("service") == "DCR":
                    values.append(item)
                    if not item.get("validity", {}).get("semantics", False):
                        decode_failures += 1
                    if not item.get("validity", {}).get("l1s_crc", False):
                        crc_failures += 1
    else:
        reassembler = NankaiTroughReassembler(capacity=128, timeout_seconds=7200)
        for record in RawArchiveReader(str(path)):
            if record.frame.frame_type != FrameType.L1S_FRAME:
                continue
            try:
                claim = parse_payload(record.frame)
                comparison = compare_claim(claim)
            except ValueError:
                decode_failures += 1
                continue
            if comparison.verified is None or comparison.mismatch:
                decode_failures += 1
                continue
            if not comparison.verified.valid_crc:
                crc_failures += 1
                continue
            if comparison.verified.message_type != 43:
                continue
            alert = make_alert(record.frame, claim, comparison.verified,
                               dcr_reassembler=reassembler)
            values.append(json_safe(alert))
            if not alert.validity.semantics:
                decode_failures += 1
    return values, decode_failures, crc_failures


def _parse_time(value):
    if not value:
        return None
    try:
        return datetime.fromisoformat(str(value).replace("Z", "+00:00"))
    except ValueError:
        return None


def _message_time(message, fields):
    return _parse_time(fields.get("report_time"))


def _expected_time(message, observed):
    for value in message["At_utc_by_run"].values():
        expected = _parse_time(value)
        if expected and observed and abs((observed - expected).total_seconds()) <= 60:
            return True
    return False


def _as_list(value):
    return list(value) if isinstance(value, (tuple, list)) else []


def _match_details(message, candidates):
    expected = message.get("expected", {})
    failures = []
    if message.get("action") and not any(item.get("action") == message["action"]
                                         for item, _ in candidates):
        failures.append("action")
    def any_field(name, expected_value):
        return any(fields.get(name) == expected_value for _item, fields in candidates)

    scalar_map = {
        "warning_code_raw": "tsunami_warning_code_raw",
        "volcano_raw": "volcano_name_raw",
        "warning_type_raw": "ash_fall_warning_type_raw",
        "warning_state_raw": "weather_warning_state_raw",
        "information_serial_code_raw": "information_serial_code_raw",
    }
    for key, field in scalar_map.items():
        value = expected.get(key, message.get("common_fields", {}).get(key))
        if value is not None and not any_field(field, value):
            failures.append(key)

    entries = expected.get("entries")
    if entries:
        if message["Dc"] == 5:
            actual = {(height, region) for _item, fields in candidates
                      for height, region in zip(_as_list(fields.get("tsunami_heights_raw")),
                                                _as_list(fields.get("tsunami_forecast_regions_raw")))}
            wanted = {(entry["height_raw"], entry["region_raw"]) for entry in entries}
        elif message["Dc"] == 11:
            actual = {(level, region) for _item, fields in candidates
                      for level, region in zip(_as_list(fields.get("flood_warning_levels_raw")),
                                               _as_list(fields.get("flood_forecast_regions_raw")))}
            wanted = {(entry["warning_level_raw"], entry["forecast_region_raw"])
                      for entry in entries}
        else:
            actual = wanted = set()
        if actual != wanted:
            failures.append("entries")

    ordered_entries = expected.get("entries_in_order")
    if ordered_entries and message["Dc"] == 9:
        actual = {(hour, warning, municipality) for _item, fields in candidates
                  for hour, warning, municipality in zip(
                      _as_list(fields.get("expected_ash_fall_times")),
                      _as_list(fields.get("ash_fall_warning_codes_raw")),
                      _as_list(fields.get("local_governments_raw")))}
        wanted = {(entry["forecast_hour"], entry["warning_code_raw"],
                   entry["municipality_raw"]) for entry in ordered_entries}
        if actual != wanted:
            failures.append("ash_entries")
        unique_parts = {json.dumps({key: fields.get(key) for key in (
            "expected_ash_fall_times", "ash_fall_warning_codes_raw",
            "local_governments_raw")}, sort_keys=True)
                        for _item, fields in candidates}
        if len(unique_parts) < message.get("expected_parts", 1):
            failures.append("missing_part")
    if ordered_entries and message["Dc"] == 10:
        actual = {(sub, region) for _item, fields in candidates
                  for sub, region in zip(
                      _as_list(fields.get("weather_related_disaster_sub_categories_raw")),
                      _as_list(fields.get("weather_forecast_regions_raw")))}
        wanted = {(entry["sub_category_raw"], entry["forecast_region_raw"])
                  for entry in ordered_entries}
        if actual != wanted:
            failures.append("weather_entries")

    if message["Dc"] == 4:
        pages = {int(fields.get("page_number", 0) or 0): fields
                 for _item, fields in candidates}
        total = expected.get("expected_pages")
        if total and set(pages) != set(range(1, total + 1)):
            failures.append("missing_page")
        if pages and any(int(fields.get("total_page", 0) or 0) != total
                         for fields in pages.values()):
            failures.append("page_total")
        chunks = []
        for page in sorted(pages):
            value = pages[page].get("text_information", "")
            try:
                chunks.append(bytes.fromhex(value) if isinstance(value, str) else bytes(value))
            except (ValueError, TypeError):
                failures.append("utf8_page")
        try:
            text = b"".join(chunks).decode("utf-8")
        except UnicodeDecodeError:
            text = ""
            failures.append("utf8_boundary")
        if pages and text != expected.get("text_utf8"):
            failures.append("reassembled_text")
    return sorted(set(failures))


def validate(path: Path, manifest: dict, input_format: str):
    observations, decode_failures, crc_failures = _observations(path, input_format)
    test = [item for item in observations if item.get("status") == "test"]
    actual = [item for item in observations if item.get("status") == "actual"]
    results = []
    for scenario in manifest["scenarios"]:
        for message in scenario["messages"]:
            candidates = []
            timing_deviation = 0
            for item in test:
                if item.get("category", {}).get("code") != message["Dc"]:
                    continue
                fields = _fields(item)
                report_time = _message_time(message, fields)
                if _expected_time(message, report_time):
                    candidates.append((item, fields))
                else:
                    timing_deviation += 1
            if not candidates:
                status = "not_received"
                failures = []
            else:
                failures = _match_details(message, candidates)
                status = "received_and_valid" if not failures else "received_and_field_mismatch"
            results.append({"scenario": scenario["id"], "message": message["id"],
                            "category": message["Dc"], "status": status,
                            "received": len(candidates), "duplicates": max(0, len(candidates) - 1),
                            "field_mismatches": failures,
                            "timing_deviation": timing_deviation,
                            "preemption_possible": status == "not_received"})
    states = Counter(item["status"] for item in results)
    by_message = {item["message"]: item for item in results}
    lifecycle = []
    for group in manifest.get("lifecycle_groups", []):
        members = [by_message[item] for item in group["message_ids"]]
        lifecycle.append({"id": group["id"],
                          "identity_confidence": group["identity_confidence"],
                          "expected_actions": group["expected_actions"],
                          "message_statuses": {item["message"]: item["status"]
                                               for item in members},
                          "complete": all(item["status"] == "received_and_valid"
                                          for item in members)})
    report_times = [value for item in observations
                    if (value := _fields(item).get("report_time"))]
    if decode_failures:
        verdict = "fail"
    elif states["received_and_field_mismatch"]:
        verdict = "fail"
    elif states["received_and_valid"] and states["not_received"]:
        verdict = "partial"
    elif states["received_and_valid"]:
        verdict = "pass"
    else:
        verdict = "not_observed"
    return {"schema": "hoshiguard.dcr_scenario_validation.v1",
            "source": str(path), "input_format": input_format,
            "session_range": {"first_report_time": min(report_times, default=None),
                              "last_report_time": max(report_times, default=None)},
            "received_test_messages": len(test),
            "actual_messages_mixed_in": len(actual),
            "decode_failures": decode_failures, "crc_failures": crc_failures,
            "category_coverage": sorted({item.get("category", {}).get("code")
                                         for item in test}),
            "status_counts": {**dict(states),
                              "received_but_decode_failed": decode_failures},
            "distinctions": ["not_received", "received_but_decode_failed",
                             "received_and_field_mismatch", "received_and_valid"],
            "messages": results, "lifecycle_groups": lifecycle,
            "not_received_is_not_decoder_failure": True,
            "final_verdict": verdict}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input")
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--input-format", choices=("hgraw", "ndjson"))
    parser.add_argument("--json-report")
    args = parser.parse_args()
    input_format = args.input_format or ("ndjson" if args.input.endswith(".ndjson")
                                         else "hgraw")
    manifest = json.loads(Path(args.manifest).read_text(encoding="utf-8"))
    report = validate(Path(args.input), manifest, input_format)
    if args.json_report:
        destination = Path(args.json_report)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(report, ensure_ascii=False, indent=2,
                                          sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True))
    return 1 if report["final_verdict"] == "fail" else 0


if __name__ == "__main__":
    raise SystemExit(main())
