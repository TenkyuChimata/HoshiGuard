#!/usr/bin/env python3
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    dcx = (ROOT / "docs/dcx_mt44_traceability.md").read_text(encoding="utf-8")
    dcr = (ROOT / "docs/dcr_mt43_traceability.md").read_text(encoding="utf-8")
    missing = [name for name in [*(f"A{i}" for i in range(1, 19)),
                                  *(f"EX{i}" for i in range(1, 12))]
               if name not in dcx]
    for code in (1, 2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 14):
        if not re.search(rf"\|\s*{code}\s*\|", dcr):
            missing.append(f"DCR category {code}")
    camf = (ROOT / "python/hoshiguard/dcx/camf.py").read_text(encoding="utf-8")
    for index in range(1, 19):
        if f'"A{index}"' not in camf:
            missing.append(f"CAMF_TRACE A{index}")
    if missing:
        print("untraced fields: " + ", ".join(missing), file=sys.stderr)
        return 1
    print("alert traceability OK: DCR active categories, A1-A18, EX1-EX11")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

