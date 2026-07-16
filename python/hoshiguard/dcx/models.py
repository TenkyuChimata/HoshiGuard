from dataclasses import dataclass
from typing import Any


@dataclass(frozen=True)
class CamfFields:
    a1_message_type: int
    a2_country_region: int
    a3_provider: int
    a4_hazard: int
    a5_severity: int
    a6_onset_week: int
    a7_onset_minute_of_week: int
    a8_duration: int
    a9_library_selection: int
    a10_library_version: int
    a11_guidance: int
    a12_latitude: int
    a13_longitude: int
    a14_semi_major: int
    a15_semi_minor: int
    a16_azimuth: int
    a17_specific_kind: int
    a18_specific_settings: int


@dataclass(frozen=True)
class ExtendedFields:
    variant: str
    raw_fields: tuple[tuple[str, Any], ...]
    version: int
    blocked_by_missing_spec: tuple[str, ...] = ()


@dataclass(frozen=True)
class DcxPayload:
    camf: CamfFields
    extended: ExtendedFields
    decoded_fields: tuple[tuple[str, Any], ...]

