"""UnifiedAlert の公開用投影。伝送証跡を含めず、再デコードしない。"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
import re
from typing import Any

from .dcr.models import DcrPayload
from .dcx.models import DcxPayload
from .dcx.guidance import guidance_code
from .models import UnifiedAlert, json_safe

PUBLIC_SCHEMA = "hoshiguard.public-alert.v1"
_PRIVATE_FIELDS = frozenset({
    "message", "message_header", "message_type", "nmea", "preamble", "raw",
    "satellite_id", "satellite_prn", "sentence", "timestamp", "version",
})
_DATE = re.compile(r"^\d{4}-\d\d-\d\d[T ]\d\d:\d\d")
_DCR_AREAS = (
    "eew_forecast_regions", "prefectures", "tsunami_forecast_regions",
    "coastal_regions", "local_governments", "weather_forecast_regions",
    "flood_forecast_regions", "marine_forecast_regions",
)


def utc_iso(ns: int) -> str:
    return datetime.fromtimestamp(ns / 1_000_000_000, timezone.utc).isoformat()


def _utc_dates(value: Any) -> Any:
    """vendor のタイムゾーンなし DCR 日時は UTC を示し、端末のローカル時刻ではない。"""
    if isinstance(value, str) and _DATE.match(value):
        parsed = datetime.fromisoformat(value)
        return parsed.replace(tzinfo=timezone.utc).isoformat() if parsed.tzinfo is None else parsed.astimezone(timezone.utc).isoformat()
    if isinstance(value, dict):
        return {key: _utc_dates(item) for key, item in value.items()}
    if isinstance(value, (tuple, list)):
        return [_utc_dates(item) for item in value]
    return value


@dataclass(frozen=True)
class PublicAlert:
    schema: str
    id: str
    source: dict
    service: str
    category: dict
    status: str
    action: str
    identity: dict
    times: dict
    areas: list
    severity: dict | None
    payload: dict
    validity: dict
    is_real_disaster: bool
    provenance: dict

    @classmethod
    def from_alert(cls, alert: UnifiedAlert, received_ns: int, *, replay: bool = False) -> PublicAlert:
        if not alert.alert:
            raise ValueError("service observations are not public business alerts")
        identifier = f"sf{alert.identity.semantic_fingerprint_version}-{alert.identity.semantic_fingerprint}"
        areas = json_safe(alert.areas)
        source = {"name": "QZSS L1S", "type": "satellite_broadcast", "provider": None}
        severity = json_safe(alert.severity)
        status = alert.status
        times = {"issued": None, "received": utc_iso(received_ns),
                 "onset": None, "expires": None, "basis": "not_carried"}
        if isinstance(alert.payload, DcrPayload):
            fields = {name: _utc_dates(value) for name, value in alert.payload.fields
                      if name not in _PRIVATE_FIELDS}
            if fields.get("assumptive") is True:
                # DCR-017 表 4.1.2-5: De=10/Ma=10 は推定震源の固定マーカーであり、実測 10 km / M1.0 ではない。
                fields["depth_of_hypocenter"] = None
                fields["magnitude"] = None
                fields["measurement_basis"] = "assumptive_hypocenter_placeholders"
            source["provider"] = {"name": "Japan Meteorological Agency", "code": None}
            times.update(issued=_utc_dates(alert.times.issued),
                         onset=fields.get("occurrence_time_of_earthquake"),
                         basis="UTC; year inferred from receipt context",
                         issued_raw=fields.get("report_time_raw"))
            for name in _DCR_AREAS:
                codes, names = fields.get(name + "_raw", ()), fields.get(name, ())
                if isinstance(codes, (tuple, list)):
                    areas.extend({"code": code, "name": names[i] if i < len(names) else None,
                                  "kind": name} for i, code in enumerate(codes))
            payload = {"type": alert.category.token.lower() if alert.category.token else "unknown",
                       "fields": fields}
            level_fields = ("seismic_intensity_lower_limit", "seismic_intensity_upper_limit",
                            "tsunami_warning_code", "volcanic_warning_code", "flood_warning_levels",
                            "weather_warning_state", "marine_warning_codes", "typhoon_intensity_category")
            levels = {name: {"code": fields.get(name + "_raw"), "name": fields.get(name)}
                      for name in level_fields if name + "_raw" in fields}
            severity = levels or None
        elif isinstance(alert.payload, DcxPayload):
            camf = alert.payload.camf
            if alert.category.code in (13, 113):
                # 明示的な siren/test 災害は A1=Alert でも公開の test 範囲に入れる。A1 は raw CAMF に保持する。
                status = "test"
            decoded = dict(alert.payload.decoded_fields)
            source["provider"] = {"code": camf.a3_provider,
                                   "name": decoded.get("a3_provider_identifier"),
                                   "country_region_code": camf.a2_country_region}
            # CAMF は相対週・分を持ち、絶対日付付きの発表時刻は持たない。
            times.update(onset_raw={"week": camf.a6_onset_week,
                                    "minute_of_week": camf.a7_onset_minute_of_week},
                         duration_code=camf.a8_duration)
            severity = {"code": camf.a5_severity,
                        "name": ("unknown", "moderate", "severe", "extreme")[camf.a5_severity]}
            extension_fields = dict(alert.payload.extended.raw_fields)
            if "EX9_target_area_code_list" in extension_fields:
                # JSON 数値で 64 bit 全体を保持できないクライアント（JavaScript 等）向けの正確な表現。
                extension_fields["EX9_target_area_code_list_hex"] = f'{extension_fields["EX9_target_area_code_list"]:016X}'
            payload = {"type": alert.category.token.lower() if alert.category.token else "unknown",
                       "camf": json_safe(camf),
                       "guidance": guidance_code(decoded, camf.a11_guidance),
                       "extended": {"variant": alert.payload.extended.variant,
                                    "version": alert.payload.extended.version,
                                    "fields": extension_fields},
                       "limitations": list(alert.payload.extended.blocked_by_missing_spec)}
        else:
            raise ValueError("only DCR/DCX business alerts have a public projection")
        return cls(
            PUBLIC_SCHEMA, identifier, source, alert.service.value,
            json_safe(alert.category), status, alert.action,
            {"scope": "semantic_message_content", "event_id": None,
             "semantic_fingerprint": alert.identity.semantic_fingerprint,
             "semantic_fingerprint_version": alert.identity.semantic_fingerprint_version,
             "correlation_hint": alert.identity.event_key,
             "correlation_confidence": alert.identity.confidence},
            times, areas, severity, payload,
            {"transport_verified": all((alert.validity.bridge_crc, alert.validity.ubx,
                                        alert.validity.l1s_crc, alert.validity.structure)),
             "semantics": alert.validity.semantics,
             "complete": alert.validity.complete,
             "category_known": alert.category.known,
             "category_reserved": alert.category.reserved,
             "unknown_fields": json_safe(alert.unknown_fields)},
            alert.alert and alert.status == "actual" and alert.category.known and not alert.category.reserved
            and all((alert.validity.bridge_crc, alert.validity.ubx, alert.validity.l1s_crc,
                     alert.validity.structure, alert.validity.semantics))
            and not (alert.service.value == "DCX" and alert.category.code in (13, 113)),
            {"spec": alert.spec, "input": "replay" if replay else "live",
             "format": "QZSS DCR/DCX; not original JMA XML"},
        )
