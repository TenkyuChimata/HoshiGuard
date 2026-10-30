"""版付きで伝送に依存しない意味識別。

正規化した表現はデコード済みモデルから構築し、UBX/L1S/Bridge 証跡から作らない。
集合と明示した値（現在は ``areas`` と未知フィールド）以外はリストの順序を保持する。
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

# azarashi は意味コードと表示・伝送補助値を同じ mapping に含める。
# 補助値は QZSS の業務的意味ではなく、PAB や再生時刻による fingerprint 変化の原因となるため除外する。
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
        # decoded_fields は NMEA 文、preamble、デコード時の壁時計等の表示補助値を含む。
        # CAMF A1..A18 と版付き ExtendedFields を意味・原コードの完全な情報源とする。
        return {"camf": _normalise(value.camf),
                "extended": _normalise(value.extended)}
    return _normalise(value)


def _dcr_value(value: Any) -> Any:
    """仕様上の月・日・時刻を保持し、推定した西暦年を除く。"""
    if isinstance(value, str) and re.match(r"^\d{4}-\d\d-\d\d[T ]", value):
        return "--" + value[5:]
    if isinstance(value, Mapping):
        return {str(key): _dcr_value(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_dcr_value(item) for item in value]
    return value


def canonical_semantic(alert: UnifiedAlert) -> dict[str, Any]:
    """受信証跡を除外した正規の業務的意味を返す。"""
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
        # 017 は 016 の wire layout を維持する。既存 v2 identity の名前空間を固定し、仕様ラベル変更だけで全通報が再公開されることを防ぐ。
        # 新規に定義された意味フィールドは hash に含める。
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
        # DCR の年は受信文脈からの推定。仕様上の月・日・時・分とその他の時刻原コードは payload に保持する。
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
