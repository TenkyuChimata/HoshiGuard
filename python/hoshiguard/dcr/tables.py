"""DCR-016 のカテゴリ情報。詳細テーブルは出所を明示した vendor 配下に置く。"""

from ..models import AlertCode, UnknownField
from .._vendor.azarashi.qzss_dcr_lib import definition

_CODE_FIELDS = {
    "eew_forecast_regions": "eew_forecast_region",
    "prefectures": "prefecture",
    "seismic_intensities": "seismic_intensity",
    "seismic_epicenter": "epicenter_and_hypocenter",
    "tsunami_forecast_regions": "tsunami_forecast_region",
    "tsunami_warning_code": "tsunami_warning_code",
    "tsunami_heights": "tsunami_height",
    "coastal_regions": "coastal_region_en",
    "volcanic_warning_code": "volcanic_warning_code",
    "volcano_name": "volcano_name",
    "local_governments": "local_government",
    "weather_forecast_regions": "weather_forecast_region",
    "weather_warning_state": "weather_warning_state",
    "weather_related_disaster_sub_categories": "weather_related_disaster_sub_category",
    "flood_forecast_regions": "flood_forecast_region",
    "flood_warning_levels": "flood_warning_level",
    "marine_forecast_regions": "marine_forecast_region",
    "marine_warning_codes": "marine_warning_code",
    "notifications_on_disaster_prevention": "notification_on_disaster_prevention",
    "information_serial_code": "information_serial_code",
    "ash_fall_warning_codes": "ash_fall_warning_code",
    "reference_time_type": "typhoon_reference_time_type",
    "typhoon_scale_category": "typhoon_scale_category",
    "typhoon_intensity_category": "typhoon_intensity_category",
    "tsunamigenic_potential": "tsunamigenic_potential_en",
    "seismic_intensity_lower_limit": "seismic_intensity_lower_limit",
    "seismic_intensity_upper_limit": "seismic_intensity_upper_limit",
    "long_period_ground_motion_lower_limit": "long_period_ground_motion_lower_limit",
    "long_period_ground_motion_upper_limit": "long_period_ground_motion_upper_limit",
}


def unknown_codes(fields: dict) -> tuple[UnknownField, ...]:
    result = []
    for field, table_name in _CODE_FIELDS.items():
        raw = fields.get(field + "_raw")
        if raw is None:
            continue
        if field == "tsunami_heights" and fields.get("disaster_category_no") == 6:
            table_name = "northwest_pacific_tsunami_height_en"
        table = getattr(definition, "qzss_dcr_jma_" + table_name)
        for value in raw if isinstance(raw, (tuple, list)) else (raw,):
            reason = ("undefined_code" if value not in table else
                      "reserved_code" if str(table[value]).lower() == "reserved" else None)
            if reason:
                result.append(UnknownField(field, value, reason=reason))
    return tuple(result)

_CATEGORIES = {
    1: ("EARTHQUAKE_EARLY_WARNING", "緊急地震速報", "Earthquake Early Warning", "紧急地震速报"),
    2: ("HYPOCENTER", "震源", "Hypocenter", "震源"),
    3: ("SEISMIC_INTENSITY", "震度", "Seismic Intensity", "震度"),
    4: ("NANKAI_TROUGH_EARTHQUAKE", "南海トラフ地震", "Nankai Trough Earthquake", "南海海槽地震"),
    5: ("TSUNAMI", "津波", "Tsunami", "海啸"),
    6: ("NORTHWEST_PACIFIC_TSUNAMI", "北西太平洋津波", "Northwest Pacific Tsunami", "西北太平洋海啸"),
    8: ("VOLCANO", "火山", "Volcano", "火山"),
    9: ("ASH_FALL", "降灰", "Ash Fall", "降灰"),
    10: ("WEATHER", "気象", "Weather", "气象"),
    11: ("FLOOD", "洪水", "Flood", "洪水"),
    12: ("TYPHOON", "台風", "Typhoon", "台风"),
    14: ("MARINE", "海上", "Marine", "海上"),
}


def category(code: int) -> AlertCode:
    item = _CATEGORIES.get(code)
    if item:
        return AlertCode(code, item[0], True, False, item[1], item[2],
                         item[3], "hoshiguard")
    return AlertCode(code, None, False, code in (0, 7, 13, 15))


def supported_categories() -> tuple[int, ...]:
    return tuple(_CATEGORIES)
