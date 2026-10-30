"""Versioned transport-independent semantic identity.

The canonical representation is deliberately built from the decoded model,
not from UBX/L1S/Bridge evidence.  Lists are kept ordered unless the model
explicitly says that they are sets (currently ``areas`` and unknown fields).
"""

from __future__ import annotations

from dataclasses import asdict, is_dataclass
from enum import Enum
from hashlib import sha256
import json
import re
from typing import Any, Mapping

from ..dcr.models import DcrPayload
from ..dcx.models import DcxPayload
from ..models import UnifiedAlert, json_safe

SEMANTIC_FINGERPRINT_VERSION = 2

# azarashi exposes useful semantic codes and presentation/transport helpers in
# one mapping.  These fields are not QZSS business semantics and were the root
# cause of v0.2.5 fingerprints changing with PAB and replay time.
_DCR_NON_SEMANTIC_FIELDS = frozenset({
    "message", "message_header", "message_type", "nmea", "preamble",
    "raw", "satellite_id", "satellite_prn", "sentence", "timestamp",
    "version", "disaster_category", "disaster_category_en",
    "information_type", "information_type_en", "report_classification",
    "report_classification_en", "marine_forecast_regions",
    "marine_warning_codes",
})


def _normalise(value: Any) -> Any:
    if is_dataclass(value):
        return _normalise(asdict(value))
    if isinstance(value, Enum):
        return value.value
    if isinstance(value, bytes):
        return {"bytes_hex": value.hex().upper()}
    if isinstance(value, Mapping):
        return {str(key): _normalise(value[key])
                for key in sorted(value, key=lambda item: str(item))}
    if isinstance(value, tuple):
        return [_normalise(item) for item in value]
    if isinstance(value, list):
        return [_normalise(item) for item in value]
    if isinstance(value, (str, int, bool)) or value is None:
        return value
    if isinstance(value, float):
        if value != value or value in (float("inf"), float("-inf")):
            raise ValueError("non-finite semantic number")
        return value
    return json_safe(value)


def _payload(value: Any) -> Any:
    if isinstance(value, DcrPayload):
        fields = {
            name: _dcr_value(item) for name, item in value.fields
            if name not in _DCR_NON_SEMANTIC_FIELDS
        }
        return {
            "payload_type": type(value).__name__,
            "disaster_category": value.disaster_category,
            "fields": _normalise(fields),
        }
    if isinstance(value, DcxPayload):
        # decoded_fields contains vendor presentation helpers (NMEA sentence,
        # preamble and wall-clock decode timestamp). CAMF A1..A18 plus the
        # versioned ExtendedFields are the complete semantic/raw-code source.
        return {"camf": _normalise(value.camf),
                "extended": _normalise(value.extended)}
    return _normalise(value)


def _dcr_value(value: Any) -> Any:
    """Remove inferred Gregorian year while retaining normative month/day/time."""
    if isinstance(value, str) and re.match(r"^\d{4}-\d\d-\d\d[T ]", value):
        return "--" + value[5:]
    if isinstance(value, Mapping):
        return {str(key): _dcr_value(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_dcr_value(item) for item in value]
    return value


def canonical_semantic(alert: UnifiedAlert) -> dict[str, Any]:
    """Return canonical business meaning, excluding all receipt evidence."""
    areas = sorted(({
        "code": _normalise(area.code),
        "kind": area.kind,
        "raw": _normalise(area.raw),
    } for area in alert.areas), key=lambda item: canonical_json(item))
    unknown = sorted(({
        "name": field.name,
        "raw_value": _normalise(field.raw_value),
        "bit_offset": field.bit_offset,
        "bit_width": field.bit_width,
        "reason": field.reason,
    } for field in alert.unknown_fields), key=lambda item: canonical_json(item))
    payload = _payload(alert.payload)
    if isinstance(alert.payload, Mapping) and "raw_payload_hex" in alert.payload:
        bits = "".join(f"{byte:08b}" for byte in bytes.fromhex(alert.raw.l1s_hex))
        payload = {"l1s_semantic_bits_14_225": bits[14:226]}
    return {
        "semantic_fingerprint_version": SEMANTIC_FINGERPRINT_VERSION,
        "service": alert.service.value,
        # 017 retains the 016 wire layout. Pin this existing v2 identity
        # namespace so a specification-label update cannot republish every
        # unchanged observation. Newly defined semantic fields still hash.
        "spec": "IS-QZSS-DCR-016" if alert.service.value == "DCR" and alert.spec == "IS-QZSS-DCR-017" else alert.spec,
        "status": alert.status,
        "action": alert.action,
        "alert": alert.alert,
        "category": {
            "code": alert.category.code,
            "token": alert.category.token,
            "known": alert.category.known,
            "reserved": alert.category.reserved,
        },
        "severity": _normalise(alert.severity),
        # DCR's year is receiver-context inference. Its normative raw
        # month/day/hour/minute and all other time codes remain in payload.
        "times": None if alert.service.value == "DCR" else _normalise(alert.times),
        "areas": areas,
        "payload": payload,
        "unknown_fields": unknown,
    }


def canonical_json(value: Any) -> str:
    return json.dumps(_normalise(value), ensure_ascii=False, sort_keys=True,
                      separators=(",", ":"), allow_nan=False)


def semantic_fingerprint(alert: UnifiedAlert) -> str:
    return sha256(canonical_json(canonical_semantic(alert)).encode("utf-8")).hexdigest()
