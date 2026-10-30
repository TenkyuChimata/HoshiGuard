"""MT43 decoder wrapper with the DCR-017 additive code/time updates.

The exhaustive field/table implementation is the MIT-licensed azarashi 0.16.3
semantic core (vendored with its license). HoshiGuard independently validates
UBX/L1S before invoking it and preserves every raw field in its canonical model.
"""

from __future__ import annotations

from datetime import datetime, timezone
from typing import Any

from .._vendor.azarashi.qzss_dcr_lib.decoder.hex_qzss_dcr_decoder import HexQzssDcrDecoder
from ..l1s import VerifiedL1s, read_bits
from ..models import json_safe
from .models import PAYLOAD_TYPES, DcrPayload
from .reassembly import NankaiTroughReassembler
from .tables import category


class DcrDecodeError(ValueError):
    pass


def _freeze_mapping(mapping: dict[str, Any]) -> tuple[tuple[str, Any], ...]:
    safe = json_safe(mapping)
    return tuple(sorted(safe.items(), key=lambda item: item[0]))


def decode_dcr(frame: VerifiedL1s, receipt_time: datetime | None = None,
               reassembler: NankaiTroughReassembler | None = None):
    if frame.message_type != 43:
        raise DcrDecodeError("not MT43")
    disaster_category = read_bits(frame.raw, 17, 4)
    if receipt_time is None:
        receipt_time = datetime.now(timezone.utc)
    try:
        report = HexQzssDcrDecoder(
            sentence=frame.raw.hex().upper()[:63], timestamp=receipt_time
        ).decode()
    except Exception as exc:  # vendor exposes several typed decoder exceptions
        raise DcrDecodeError(str(exc)) from exc
    fields = report.get_params()
    fields.pop("message", None)
    fields.pop("raw", None)
    fields["report_time_raw"] = {
        "month": read_bits(frame.raw, 21, 4),
        "day": read_bits(frame.raw, 25, 5),
        "hour": read_bits(frame.raw, 30, 5),
        "minute": read_bits(frame.raw, 35, 6),
    }
    if disaster_category == 5:
        fields["expected_tsunami_arrival_times_raw"] = [
            {"day": read_bits(frame.raw, 84 + i * 26, 1),
             "hour": read_bits(frame.raw, 85 + i * 26, 5),
             "minute": read_bits(frame.raw, 90 + i * 26, 6)}
            for i in range(len(fields.get("expected_tsunami_arrival_times", ())))]
    if disaster_category == 4:
        page = int(fields.get("page_number", 0) or 0)
        total = int(fields.get("total_page", 0) or 0)
        text = fields.get("text_information", b"")
        if isinstance(text, str):
            text = text.encode("utf-8", "replace")
        key = (fields.get("report_time"),
               fields.get("report_classification_no"),
               fields.get("information_type_no"),
               fields.get("information_serial_code_raw"))
        assembled = (reassembler.add(key, page, total, bytes(text))
                     if reassembler is not None else
                     bytes(text) if page == 1 and total == 1 else None)
        fields["reassembly_complete"] = assembled is not None
        fields["reassembled_text_hex"] = (
            assembled.hex().upper() if assembled is not None else None)
    payload_type = PAYLOAD_TYPES.get(disaster_category, DcrPayload)
    payload = payload_type(disaster_category, _freeze_mapping(fields))
    information_type = int(fields.get("information_type_no", 0) or 0)
    action = {0: "issue", 1: "update", 2: "cancel"}.get(information_type,
                                                           "unknown")
    # DCR-016 service-specific clear values are semantic lifecycle actions even
    # when the generic Information Type remains "issue".  The July 2026
    # official scenario oracle exercises both cases.
    if (action != "cancel" and disaster_category == 5 and
            int(fields.get("tsunami_warning_code_raw", -1)) == 2):
        action = "clear"
    if action != "cancel" and disaster_category == 11:
        levels = fields.get("flood_warning_levels_raw", ())
        if levels and all(int(value) == 1 for value in levels):
            action = "clear"
    if action != "cancel" and disaster_category == 10 and fields.get("weather_warning_state_raw") == 2:
        action = "clear"
    classification = fields.get("report_classification_no")
    status = "test" if classification == 7 else "actual" if classification in (1, 2, 3) else "unknown"
    return payload, category(disaster_category), status, action, fields
