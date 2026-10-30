"""Canonical alert model shared by public projections and diagnostic outputs."""

from __future__ import annotations

from dataclasses import asdict, dataclass, is_dataclass
from datetime import date, datetime
from enum import Enum
from hashlib import sha256
from typing import Any, Mapping


class AlertService(str, Enum):
    DCR = "DCR"
    DCX = "DCX"
    SLAS = "SLAS"
    NULL = "NULL"
    TEST = "TEST"
    RESERVED = "RESERVED"
    UNKNOWN = "UNKNOWN"


@dataclass(frozen=True)
class AlertReceipt:
    bridge_sequence: int
    timestamp_us: int
    pps_sequence: int
    sv_id: int
    pab: int
    mt: int


@dataclass(frozen=True)
class AlertValidity:
    bridge_crc: bool
    ubx: bool
    l1s_crc: bool
    structure: bool
    semantics: bool
    complete: bool


@dataclass(frozen=True)
class AlertCode:
    code: int
    token: str | None
    known: bool
    reserved: bool = False
    official_ja: str | None = None
    official_en: str | None = None
    chinese: str | None = None
    translation_source: str | None = None


@dataclass(frozen=True)
class AlertIdentity:
    frame_fingerprint: str
    semantic_fingerprint: str
    semantic_fingerprint_version: int
    event_key: str
    confidence: str


@dataclass(frozen=True)
class AlertTimes:
    issued: str | None = None
    onset: str | None = None
    expires: str | None = None
    raw: tuple[tuple[str, Any], ...] = ()


@dataclass(frozen=True)
class AlertArea:
    code: int | str
    name: str | None
    kind: str
    raw: Any = None


@dataclass(frozen=True)
class UnknownField:
    name: str
    raw_value: Any
    bit_offset: int | None = None
    bit_width: int | None = None
    reason: str = "unknown"


@dataclass(frozen=True)
class RawEvidence:
    l1s_hex: str
    ubx_hex: str
    received_crc: str
    calculated_crc: str
    spare_bits: int
    extra_word_hex: str | None


@dataclass(frozen=True)
class UnifiedAlert:
    schema: str
    source: str
    service: AlertService
    spec: str
    receipt: AlertReceipt
    validity: AlertValidity
    status: str
    action: str
    identity: AlertIdentity
    category: AlertCode
    severity: Mapping[str, Any] | None
    times: AlertTimes
    areas: tuple[AlertArea, ...]
    payload: Any
    unknown_fields: tuple[UnknownField, ...]
    raw: RawEvidence
    alert: bool = True


def json_safe(value: Any) -> Any:
    if is_dataclass(value):
        return {key: json_safe(item) for key, item in asdict(value).items()}
    if isinstance(value, Enum):
        return value.value
    if isinstance(value, bytes):
        return value.hex().upper()
    if isinstance(value, (datetime, date)):
        return value.isoformat()
    if isinstance(value, Mapping):
        return {str(key): json_safe(item) for key, item in value.items()}
    if isinstance(value, tuple):
        return [json_safe(item) for item in value]
    if isinstance(value, list):
        return [json_safe(item) for item in value]
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    return str(value)


def stable_digest(value: Any) -> str:
    import json
    encoded = json.dumps(json_safe(value), ensure_ascii=False, sort_keys=True,
                         separators=(",", ":"), allow_nan=False).encode("utf-8")
    return sha256(encoded).hexdigest()
