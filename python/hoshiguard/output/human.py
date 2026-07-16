from __future__ import annotations

from ..events.state_machine import EventResult


def format_human(result: EventResult, language: str = "en",
                 detail: bool = False) -> str:
    alert = result.alert
    labels = {
        "en": ("Provider", "Hazard", "Action", "Validity", "Repeated"),
        "ja": ("提供者", "災害種別", "動作", "検証", "反復"),
        "zh": ("发布方", "灾害类别", "动作", "有效性", "重复"),
    }.get(language, ("Provider", "Hazard", "Action", "Validity", "Repeated"))
    status = alert.status.upper()
    severity = ""
    if alert.severity is not None:
        severity = f"[SEVERITY={alert.severity.get('code')}]"
    if language == "ja" and alert.category.official_ja:
        category_name = alert.category.official_ja
    elif language == "zh" and alert.category.chinese:
        category_name = alert.category.chinese
    elif alert.category.official_en:
        category_name = alert.category.official_en
    else:
        category_name = alert.category.token or "UNKNOWN"
    complete = "complete" if alert.validity.complete else "INCOMPLETE"
    lines = [
        f"[{status}][{alert.service.value}]{severity}",
        f"{labels[1]}: {category_name} "
        f"(raw={alert.category.code})",
        f"{labels[2]}: " + (result.disposition if alert.alert else alert.action),
        f"{labels[3]}: {complete}; UBX={alert.validity.ubx}; "
        f"L1S_CRC={alert.validity.l1s_crc}",
        f"{labels[4]}: {result.repeat_count} frame(s); "
        f"satellites={list(result.satellites_seen)}; "
        f"PABs={[f'{value:02X}' for value in result.pabs_seen]}",
    ]
    if hasattr(alert.payload, "camf") and alert.status != "not_applicable":
        lines.insert(1, f"{labels[0]}: raw={alert.payload.camf.a3_provider}")
    if alert.times.issued:
        lines.append(f"Issued: {alert.times.issued}")
    if alert.areas:
        rendered = ", ".join(
            f"{area.name or area.kind}:{area.code}" for area in alert.areas)
        lines.append(f"Areas: {rendered}")
    if alert.unknown_fields:
        lines.append("Unknown/reserved: " + ", ".join(
            f"{field.name}={field.raw_value}" for field in alert.unknown_fields))
    if not alert.alert:
        lines.append(f"Service observation: {result.disposition}; alert=false")
    if detail:
        lines.append(f"Event key: {alert.identity.event_key} "
                     f"(confidence={alert.identity.confidence})")
        lines.append(f"Raw L1S: {alert.raw.l1s_hex}")
    return "\n".join(lines)
