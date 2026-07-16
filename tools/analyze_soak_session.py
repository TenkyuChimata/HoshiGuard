#!/usr/bin/env python3
"""Produce segmented, time, throughput and semantic audits for a soak archive."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

from hoshiguard.soak_analysis import analyze_soak, markdown_summary  # noqa: E402


def _write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True,
                               ensure_ascii=False, allow_nan=False) + "\n",
                    encoding="utf-8")


def _simple_md(title: str, value: dict) -> str:
    return f"# {title}\n\n```json\n{json.dumps(value, indent=2, sort_keys=True)}\n```\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive")
    parser.add_argument("--json-report", default="reports/soak-24h-report.json")
    parser.add_argument("--hour-seconds", type=int, default=3600)
    args = parser.parse_args()
    report = analyze_soak(args.archive, hourly_seconds=args.hour_seconds)
    target = Path(args.json_report)
    _write_json(target, report)
    target.with_suffix(".md").write_text(markdown_summary(report), encoding="utf-8")
    directory = target.parent
    _write_json(directory / "soak-24h-hourly.json", {
        "schema": "hoshiguard.soak_hourly.v1", "source": report["source"],
        "hours": report["hourly"]})
    for key, filename, title in (
        ("time_quality", "soak-24h-time-quality", "24-hour PPS/time-quality audit"),
        ("throughput", "soak-24h-throughput", "24-hour throughput audit"),
        ("semantic", "soak-24h-semantic-analysis", "24-hour semantic audit"),
        ("events", "soak-24h-event-analysis", "24-hour event audit"),
        ("coverage", "soak-24h-coverage", "24-hour message coverage"),
    ):
        value = {"schema": f"hoshiguard.{filename.replace('-', '_')}.v1",
                 "evidence_source": "real_hardware", "source": report["source"],
                 **report[key]}
        _write_json(directory / f"{filename}.json", value)
        (directory / f"{filename}.md").write_text(
            _simple_md(title, value), encoding="utf-8")
    print(json.dumps({
        "verdict": report["final_verdict"], "records": report["records"],
        "l1s": report["l1s"]["frames"],
        "status_windows": report["status"]["window_count"],
        "failed_windows": report["status"]["failed_windows"],
        "zero_l1s_drop_proven": report["zero_l1s_drop_proven"],
    }, indent=2))
    return 0 if report["final_verdict"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
