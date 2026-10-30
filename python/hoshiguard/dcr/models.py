"""IS-QZSS-DCR-016 に基づく MT43 payload の型。"""

from dataclasses import dataclass
from typing import Any


@dataclass(frozen=True)
class DcrPayload:
    disaster_category: int
    fields: tuple[tuple[str, Any], ...]


@dataclass(frozen=True)
class EarthquakeEarlyWarningPayload(DcrPayload): pass
@dataclass(frozen=True)
class HypocenterPayload(DcrPayload): pass
@dataclass(frozen=True)
class SeismicIntensityPayload(DcrPayload): pass
@dataclass(frozen=True)
class NankaiTroughPayload(DcrPayload): pass
@dataclass(frozen=True)
class TsunamiPayload(DcrPayload): pass
@dataclass(frozen=True)
class NorthwestPacificTsunamiPayload(DcrPayload): pass
@dataclass(frozen=True)
class VolcanoPayload(DcrPayload): pass
@dataclass(frozen=True)
class AshFallPayload(DcrPayload): pass
@dataclass(frozen=True)
class WeatherPayload(DcrPayload): pass
@dataclass(frozen=True)
class FloodPayload(DcrPayload): pass
@dataclass(frozen=True)
class TyphoonPayload(DcrPayload): pass
@dataclass(frozen=True)
class MarinePayload(DcrPayload): pass

PAYLOAD_TYPES = {
    1: EarthquakeEarlyWarningPayload,
    2: HypocenterPayload,
    3: SeismicIntensityPayload,
    4: NankaiTroughPayload,
    5: TsunamiPayload,
    6: NorthwestPacificTsunamiPayload,
    8: VolcanoPayload,
    9: AshFallPayload,
    10: WeatherPayload,
    11: FloodPayload,
    12: TyphoonPayload,
    14: MarinePayload,
}

