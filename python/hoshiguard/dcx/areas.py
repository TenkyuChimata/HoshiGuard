"""対象地域の原コードを保持する補助処理。対象地域は受信機の位置ではない。"""

from __future__ import annotations

import json
from pathlib import Path

from ..models import AlertArea
from .._vendor.azarashi.qzss_dcr_lib.definition.qzss_dcr_jma_prefecture import qzss_dcr_jma_prefecture
from .models import ExtendedFields

_AREA_PATH = Path(__file__).resolve().parents[1] / "data/areas/japan-municipality-snapshot.json"
_AREA_NAMES = None


def _area_names():
    global _AREA_NAMES
    if _AREA_NAMES is None:
        data = json.loads(_AREA_PATH.read_text(encoding="utf-8"))
        _AREA_NAMES = {int(item["code"]): item.get("official_en")
                       for item in data["entries"]}
    return _AREA_NAMES


def areas_from_extended(extended: ExtendedFields) -> tuple[AlertArea, ...]:
    fields = dict(extended.raw_fields)
    if "EX1_target_area_code" in fields:
        code = fields["EX1_target_area_code"]
        return (AlertArea(code, _area_names().get(code), "target_area_code",
                          {"code": code, "name_authority":
                           "snapshot_not_independently_audited_current_JIS"}),)
    if "EX9_target_area_code_list" in fields:
        value = fields["EX9_target_area_code_list"]
        if fields.get("EX8_target_area_list_type") == 0:
            # DCX-004 図 4.2-7: 下位 47 bit は都道府県、上位 17 bit は予約値であり対象地域に変換しない。
            return tuple(AlertArea(index + 1, qzss_dcr_jma_prefecture.get(index + 1),
                                   "prefecture_code", {"mask_bit": index})
                         for index in range(47) if value & (1 << index))
        codes = ((value >> 48) & 0xFFFF, (value >> 32) & 0xFFFF,
                 (value >> 16) & 0xFFFF, value & 0xFFFF)
        return tuple(
            AlertArea(code, _area_names().get(code), "municipality_code",
                      {"code": code, "name_authority":
                       "snapshot_not_independently_audited_current_JIS"})
            for code in codes if code != 0)
    return ()
