#!/usr/bin/env python3
"""実受信 HGRAW セッションを全体読み込みせず検証する。"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

from hoshiguard.session_validation import markdown_report, validate_session


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("session")
    parser.add_argument("--ndjson")
    parser.add_argument("--json-report")
    parser.add_argument("--markdown-report")
    args = parser.parse_args()
    report = validate_session(args.session, args.ndjson)
    if args.json_report:
        path = Path(args.json_report)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(report, indent=2, ensure_ascii=False,
                                   sort_keys=True) + "\n", encoding="utf-8")
    if args.markdown_report:
        path = Path(args.markdown_report)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(markdown_report(report), encoding="utf-8")
    print(json.dumps(report, indent=2, ensure_ascii=False, sort_keys=True))
    return 0 if report["final_verdict"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
