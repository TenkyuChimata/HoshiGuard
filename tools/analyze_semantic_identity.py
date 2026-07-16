#!/usr/bin/env python3
"""Diagnose legacy/current semantic identity from an NDJSON stream."""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from hashlib import sha256
import json
from pathlib import Path

NON_SEMANTIC_DCR = {
    "message", "message_header", "message_type", "nmea", "preamble", "raw",
    "satellite_id", "satellite_prn", "sentence", "timestamp", "version",
    "disaster_category", "disaster_category_en", "information_type",
    "information_type_en", "report_classification",
    "report_classification_en", "marine_forecast_regions",
    "marine_warning_codes",
}


def digest(value) -> str:
    return sha256(json.dumps(value, ensure_ascii=False, sort_keys=True,
                             separators=(",", ":"), allow_nan=False)
                  .encode("utf-8")).hexdigest()


def canonical(item):
    payload = item.get("payload")
    if item.get("service") == "DCR" and isinstance(payload, dict):
        fields = {key: value for key, value in payload.get("fields", [])
                  if key not in NON_SEMANTIC_DCR}
        payload = {"payload_type": item.get("category", {}).get("token"),
                   "disaster_category": payload.get("disaster_category"),
                   "fields": fields}
    elif item.get("service") == "DCX" and isinstance(payload, dict):
        payload = {"camf": payload.get("camf"),
                   "extended": payload.get("extended")}
    elif isinstance(payload, dict) and "raw_payload_hex" in payload:
        raw = bytes.fromhex(item["raw"]["l1s_hex"])
        bits = "".join(f"{byte:08b}" for byte in raw)
        payload = {"l1s_semantic_bits_14_225": bits[14:226]}
    category = item.get("category", {})
    areas = sorted(({"code": area.get("code"), "kind": area.get("kind"),
                     "raw": area.get("raw")} for area in item.get("areas", [])),
                   key=lambda value: json.dumps(value, sort_keys=True))
    unknown = sorted(item.get("unknown_fields", []),
                     key=lambda value: json.dumps(value, sort_keys=True))
    return {"semantic_fingerprint_version": 2,
            "service": item.get("service"), "spec": item.get("spec"),
            "status": item.get("status"), "action": item.get("action"),
            "alert": item.get("alert"),
            "category": {key: category.get(key) for key in
                         ("code", "token", "known", "reserved")},
            "severity": item.get("severity"), "times": item.get("times"),
            "areas": areas, "payload": payload, "unknown_fields": unknown}


def analyze(path: Path) -> dict:
    objects = []
    summaries = []
    invalid = 0
    for line in path.open(encoding="utf-8"):
        if not line.strip():
            continue
        try:
            item = json.loads(line)
        except json.JSONDecodeError:
            invalid += 1
            continue
        if item.get("record_type") == "service_summary":
            summaries.append(item)
        else:
            objects.append(item)
    canonical_groups = defaultdict(list)
    events = defaultdict(set)
    for item in objects:
        key = digest(canonical(item))
        canonical_groups[key].append(item)
        identity = item.get("identity", {})
        events[str(identity.get("event_key"))].add(key)
    marine = [item for item in objects if item.get("service") == "DCR" and
              item.get("category", {}).get("code") == 14]
    marine_groups = defaultdict(list)
    for item in marine:
        marine_groups[digest(canonical(item))].append(item)
    nulls = [item for item in objects if
             item.get("category", {}).get("token") == "NULL_MESSAGE" or
             item.get("service") == "NULL"]
    marine_summaries = [item for item in summaries
                        if item.get("service") == "DCR" and
                        item.get("observation") == "MARINE"]
    null_summaries = [item for item in summaries
                      if item.get("service") == "DCX" and
                      item.get("observation") == "NULL_MESSAGE"]
    return {
        "schema": "hoshiguard.semantic_identity_diagnostic.v1",
        "source": str(path), "source_sha256": sha256(path.read_bytes()).hexdigest(),
        "invalid_lines": invalid, "semantic_observations": len(objects),
        "service_distribution": dict(Counter(item.get("service") for item in objects)),
        "unique_frame_fingerprints": len({item.get("identity", {}).get("frame_fingerprint")
                                          for item in objects}),
        "unique_recorded_semantic_fingerprints": len({
            item.get("identity", {}).get("semantic_fingerprint") for item in objects}),
        "unique_canonical_v2_payloads": len(canonical_groups),
        "unique_event_keys": len(events),
        "distinct_semantic_payloads_per_event_key": {
            key: len(values) for key, values in sorted(events.items())},
        "canonical_repeat_distribution": dict(sorted(Counter(
            len(value) for value in canonical_groups.values()).items())),
        "pab_distribution": dict(Counter(str(item.get("receipt", {}).get("pab"))
                                         for item in objects)),
        "sv_id_distribution": dict(Counter(str(item.get("receipt", {}).get("sv_id"))
                                           for item in objects)),
        "first_seen_us": min((item.get("receipt", {}).get("timestamp_us", 0)
                              for item in objects), default=None),
        "last_seen_us": max((item.get("receipt", {}).get("timestamp_us", 0)
                             for item in objects), default=None),
        "marine": {"observations": len(marine),
                   "recorded_semantic_fingerprints": len({
                       item.get("identity", {}).get("semantic_fingerprint")
                       for item in marine}),
                   "canonical_v2_payloads": len(marine_groups),
                   "repeat_counts": sorted(len(value)
                                           for value in marine_groups.values()),
                   "pabs": sorted({item.get("receipt", {}).get("pab")
                                   for item in marine}),
                   "satellites": sorted({item.get("receipt", {}).get("sv_id")
                                         for item in marine}),
                   "aggregate_repeat_counts": sorted(
                       item.get("repeat_count", 0) for item in marine_summaries),
                   "aggregate_satellites": sorted({satellite
                       for item in marine_summaries
                       for satellite in item.get("satellites_seen", [])}),
                   "aggregate_pabs": sorted({pab for item in marine_summaries
                                              for pab in item.get("pabs_seen", [])})},
        "dcx_null": {"observations": len(nulls),
                     "recorded_semantic_fingerprints": len({
                         item.get("identity", {}).get("semantic_fingerprint")
                         for item in nulls}),
                     "canonical_v2_payloads": len({digest(canonical(item))
                                                   for item in nulls}),
                     "issued_events": sum(item.get("event", {}).get("disposition") == "issued"
                                          for item in nulls),
                     "summary_records": len(null_summaries),
                     "summary_repeat_counts": [item.get("repeat_count", 0)
                                               for item in null_summaries]},
        "repeat_summary_records": len(summaries),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("ndjson")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    report = analyze(Path(args.ndjson))
    destination = Path(args.output)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(report, indent=2, ensure_ascii=False,
                                      sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2, ensure_ascii=False, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
