#!/usr/bin/env python3
"""検証済み vendor ソースから版付きの機械可読テーブルを再生成する。"""

import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

from hoshiguard.dcr.tables import _CATEGORIES
from hoshiguard._vendor.azarashi.qzss_dcr_lib.definition.qzss_dcx_camf_a3_provider_identifier import qzss_dcx_camf_a3_provider_identifier_map
from hoshiguard._vendor.azarashi.qzss_dcr_lib.definition.qzss_dcx_camf_a4_hazard_category_and_type import qzss_dcx_camf_a4_hazard_category, qzss_dcx_camf_a4_hazard_definition, qzss_dcx_camf_a4_hazard_type
from hoshiguard._vendor.azarashi.qzss_dcr_lib.definition.qzss_dcx_camf_a5_severity import qzss_dcx_camf_a5_severity
from hoshiguard._vendor.azarashi.qzss_dcr_lib.definition.qzss_dcx_camf_a11_international_library import qzss_dcx_camf_a11_international_library
from hoshiguard._vendor.azarashi.qzss_dcr_lib.definition.qzss_dcx_camf_a11_japanese_library import qzss_dcx_camf_a11_japanese_library_en, qzss_dcx_camf_a11_japanese_library_ja
from hoshiguard._vendor.azarashi.qzss_dcr_lib.definition.qzss_dcx_camf_ex1_target_area_code import qzss_dcx_ex1_target_area_code_en, qzss_dcx_ex1_target_area_code_ja


def token(value):
    return re.sub(r"[^A-Z0-9]+", "_", value.upper()).strip("_")


def write(relative, value):
    path = ROOT / "python/hoshiguard/data" / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2,
                               sort_keys=True) + "\n", encoding="utf-8")


def main():
    write("dcr/categories-016.json", {
        "spec":"IS-QZSS-DCR-016", "table":"4.1.2 disaster category",
        "entries":[{"code":code,"token":item[0],"official_ja":item[1],
                    "official_en":item[2],"known":True,"reserved":False}
                   for code,item in _CATEGORIES.items()]})
    write("dcx/a4-hazards-camf-1.2.json", {
        "spec":"CAMF-1.2", "field":"A4", "section":"2.4.1.1",
        "entries":[{"code":code,"token":token(qzss_dcx_camf_a4_hazard_type[code]),
                    "official_en":qzss_dcx_camf_a4_hazard_type[code],
                    "category":qzss_dcx_camf_a4_hazard_category[code],
                    "definition":qzss_dcx_camf_a4_hazard_definition[code],
                    "known":True,"reserved":False}
                   for code in range(1,114)]})
    write("dcx/severity-camf-1.2.json", {
        "spec":"CAMF-1.2", "field":"A5",
        "entries":[{"code":code,"token":token(value),"official_en":value,
                    "known":True,"reserved":False}
                   for code,value in qzss_dcx_camf_a5_severity.items()]})
    providers=[]
    for country, table in qzss_dcx_camf_a3_provider_identifier_map.items():
        for code,value in table.items():
            providers.append({"country_region_code":country,"code":code,
                              "token":token(value),"official_en":value,
                              "known":True,"reserved":False})
    write("dcx/providers-camf-1.2.json", {"spec":"CAMF-1.2","field":"A3",
                                           "entries":providers})
    codes=set(qzss_dcx_camf_a11_international_library)|set(qzss_dcx_camf_a11_japanese_library_en)
    write("guidance/a11-camf-1.2.json", {
        "spec":"CAMF-1.2","field":"A11","entries":[{
            "code":code,
            "international_en":qzss_dcx_camf_a11_international_library.get(code),
            "japan_en":qzss_dcx_camf_a11_japanese_library_en.get(code),
            "japan_ja":qzss_dcx_camf_a11_japanese_library_ja.get(code),
            "known":True,"reserved":False} for code in sorted(codes)]})
    area_codes=sorted(set(qzss_dcx_ex1_target_area_code_en)|set(qzss_dcx_ex1_target_area_code_ja))
    write("areas/japan-municipality-snapshot.json", {
        "spec":"DCX-004/JIS-X-0402-reference",
        "authority_status":"snapshot_not_independently_audited_current_JIS",
        "entries":[{"code":code,"official_en":qzss_dcx_ex1_target_area_code_en.get(code),
                    "official_ja":qzss_dcx_ex1_target_area_code_ja.get(code),
                    "known":True,"reserved":False} for code in area_codes]})
    print(f"exported DCR={len(_CATEGORIES)} A4=113 providers={len(providers)} "
          f"areas={len(area_codes)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

