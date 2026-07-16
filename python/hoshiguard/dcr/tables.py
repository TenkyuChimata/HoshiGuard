"""DCR-016 category metadata; detailed tables live in the attributed vendor tree."""

from ..models import AlertCode

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
