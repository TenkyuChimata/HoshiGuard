#!/usr/bin/env python3
import hashlib
import json
import re
from urllib.parse import urlsplit
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    path = ROOT / "docs/spec-inventory.json"
    inventory = json.loads(path.read_text(encoding="utf-8"))
    names = {item["name"] for item in inventory["specs"]}
    required = {"IS-QZSS-L1S", "IS-QZSS-DCR", "IS-QZSS-DCX",
                "EWSS Common Alert Message Format"}
    errors = []
    if not required <= names:
        errors.append(f"missing normative entries: {sorted(required - names)}")
    for item in inventory["specs"]:
        digest = item.get("sha256")
        if item.get("complete") and (not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest)):
            errors.append(f"{item['name']}: complete entry lacks SHA-256")
        local = item.get("local_path")
        if item.get("repository_copy") != bool(local) and "repository_copy" in item:
            errors.append(f"{item['name']}: repository_copy/local_path mismatch")
        if item.get("complete"):
            parsed = urlsplit(item.get("source", ""))
            if parsed.scheme != "https" or not parsed.netloc:
                errors.append(f"{item['name']}: missing HTTPS source")
        if local:
            source = ROOT / local
            if not source.exists():
                errors.append(f"{item['name']}: missing {local}")
            elif hashlib.sha256(source.read_bytes()).hexdigest() != digest:
                errors.append(f"{item['name']}: SHA-256 mismatch")
        if not item.get("complete") and not item.get("blocked_by_missing_spec"):
            errors.append(f"{item['name']}: incomplete entry lacks blocked marker")
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"spec inventory OK: {len(inventory['specs'])} entries; "
          f"{sum(bool(x.get('blocked_by_missing_spec')) for x in inventory['specs'])} blocked")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

