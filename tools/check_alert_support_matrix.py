#!/usr/bin/env python3
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    matrix = json.loads((ROOT / "docs/support_matrix.json").read_text())
    required_dcr = {str(code) for code in (1,2,3,4,5,6,8,9,10,11,12,14)}
    actual = set(matrix["dcr"]["categories"])
    errors = []
    if actual != required_dcr:
        errors.append(f"DCR category mismatch {sorted(actual ^ required_dcr)}")
    dcx = matrix["dcx"]
    allowed = set(matrix.get("verification_levels", []))
    required_levels = {"unsupported", "raw_only", "implemented",
                       "synthetic_verified", "official_scenario_defined",
                       "official_scenario_verified", "real_broadcast_verified"}
    if allowed != required_levels:
        errors.append("verification level vocabulary mismatch")
    for code, entry in matrix["dcr"]["categories"].items():
        if not isinstance(entry, dict) or "implementation" not in entry:
            errors.append(f"DCR {code} lacks structured implementation state")
        elif not set(entry.get("verification", ())).issubset(allowed):
            errors.append(f"DCR {code} has unknown verification level")
    for key in ("A1_A18", "A4_1_113", "EX1_EX9", "EX10", "EX11",
                "reassembly", "municipality_names"):
        if key not in dcx:
            errors.append(f"missing DCX support key {key}")
    if dcx.get("EX11", {}).get("implementation") != "raw_only" or not dcx.get("EX11", {}).get("blocked_by"):
        errors.append("EX11 must remain explicitly blocked/raw")
    if dcx.get("municipality_names", {}).get("authoritative") is not False:
        errors.append("JIS municipality names must remain non-authoritative")
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("support matrix OK: 12 DCR categories; DCX limitations explicit")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
