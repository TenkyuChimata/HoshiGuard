#!/usr/bin/env python3
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import sys
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

from hoshiguard.bridge_frames import parse_payload
from hoshiguard.bridge_protocol import FrameType
from hoshiguard.dcx.decoder import decode_dcx
from hoshiguard.l1s import compare_claim
from hoshiguard.output.raw_archive import RawArchiveReader


def main(argv=None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("archive")
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--json")
    args = parser.parse_args(argv)
    manifest = json.loads(Path(args.manifest).read_text(encoding="utf-8"))
    jst = ZoneInfo("Asia/Tokyo")
    schedule = []
    for session in manifest.get("schedule_jst", []):
        for scenario, (start, end) in session["windows"].items():
            schedule.append((
                scenario,
                datetime.fromisoformat(
                    f"{session['date']}T{start}:00").replace(tzinfo=jst),
                datetime.fromisoformat(
                    f"{session['date']}T{end}:00").replace(tzinfo=jst),
            ))
    report = {"received":0, "decoded":0, "decode_failed":0, "crc_failed":0,
              "duplicates":0, "hazards":{}, "providers":{}, "severity":{},
              "areas":{}, "extensions":{}, "unexpected_codes":[],
              "timing":{"inside_schedule":0, "outside_schedule":0,
                        "scenario_counts":{}, "maximum_deviation_seconds":0}}
    seen = set()
    for record in RawArchiveReader(args.archive):
        if record.frame.frame_type != FrameType.L1S_FRAME:
            continue
        report["received"] += 1
        try:
            claim = parse_payload(record.frame)
            comparison = compare_claim(claim)
            if comparison.mismatch or comparison.verified is None:
                report["crc_failed"] += 1
                continue
            verified = comparison.verified
            if verified.message_type != 44:
                continue
            payload, _category, status, _action, _fields, areas = decode_dcx(verified)
        except ValueError:
            report["decode_failed"] += 1
            continue
        report["decoded"] += 1
        received_at = datetime.fromtimestamp(
            record.pc_receive_ns / 1_000_000_000,
            tz=timezone.utc).astimezone(jst)
        matching = [item for item in schedule
                    if item[1] <= received_at <= item[2]]
        if matching:
            scenario = matching[0][0]
            counts = report["timing"]["scenario_counts"]
            counts[scenario] = counts.get(scenario, 0) + 1
            report["timing"]["inside_schedule"] += 1
        else:
            report["timing"]["outside_schedule"] += 1
            if schedule:
                deviation = min(
                    min(abs((received_at - start).total_seconds()),
                        abs((received_at - end).total_seconds()))
                    for _scenario, start, end in schedule)
                report["timing"]["maximum_deviation_seconds"] = max(
                    report["timing"]["maximum_deviation_seconds"],
                    int(deviation))
        fingerprint = verified.raw.hex()[:63]
        if fingerprint in seen:
            report["duplicates"] += 1
        seen.add(fingerprint)
        for target, value in (("hazards", payload.camf.a4_hazard),
                              ("providers", payload.camf.a3_provider),
                              ("severity", payload.camf.a5_severity)):
            key = str(value); report[target][key] = report[target].get(key, 0) + 1
        for area in areas:
            key = str(area.code); report["areas"][key] = report["areas"].get(key, 0) + 1
        for name, _value in payload.extended.raw_fields:
            match = re.match(r"EX(\d+)", name)
            if match:
                key = match.group(1)
                report["extensions"][key] = report["extensions"].get(key, 0) + 1
        if status != "test":
            report["unexpected_codes"].append({"field":"status","value":status})
    required = manifest["required_coverage"]
    expected_hazards = set(range(required["A4"]["start"],
                                 required["A4"]["end"] + 1))
    missing = sorted(expected_hazards - {int(x) for x in report["hazards"]})
    report["missing_expected_code"] = missing
    report["not_received"] = {"hazard_codes": missing,
                              "count": len(missing)}
    report["provider_coverage"] = sorted(int(x) for x in report["providers"])
    report["missing_providers"] = sorted(set(required["providers"]) -
                                         set(report["provider_coverage"]))
    report["missing_severity"] = sorted(set(required["severity"]) -
                                        {int(x) for x in report["severity"]})
    report["missing_extensions"] = sorted(set(required["extensions"]) -
                                          {int(x) for x in report["extensions"]})
    report["area_coverage"] = len(report["areas"])
    report["field_coverage"] = {
        "A4": 113 - len(report["missing_expected_code"]),
        "severity": len(report["severity"]),
        "provider": len(report["providers"]),
        "extensions": sorted(int(x) for x in report["extensions"]),
    }
    report["scenario_coverage"] = [
        {"id": scenario["id"],
         "received_in_scheduled_window":
             report["timing"]["scenario_counts"].get(scenario["id"], 0),
         "provider_observed": str(scenario["provider"]) in report["providers"]}
        for scenario in manifest["scenarios"]
    ]
    report["interpretation"] = (
        "not_received is coverage evidence, not decoder failure; an actual "
        "emergency may pre-empt a scheduled test slot")
    output = json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True)
    if args.json:
        Path(args.json).write_text(output + "\n", encoding="utf-8")
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
