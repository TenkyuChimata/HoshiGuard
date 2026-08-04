"""MT44/DCX-004 and CAMF Issue 1.2 decoder."""

from __future__ import annotations

from datetime import datetime, timezone
from typing import Any

from .._vendor.azarashi.qzss_dcr_lib.decoder.hex_qzss_dcr_decoder import HexQzssDcrDecoder
from .._vendor.azarashi.qzss_dcr_lib.exception import (
    QzssDcrDecoderException, QzssDcrDecoderNotImplementedError,
)
from ..l1s import VerifiedL1s
from ..models import json_safe
from .areas import areas_from_extended
from .camf import decode_camf
from .extended import decode_extended
from .models import DcxPayload
from .tables import hazard


class DcxDecodeError(ValueError):
    pass


def decode_dcx(frame: VerifiedL1s, receipt_time: datetime | None = None):
    if frame.message_type != 44:
        raise DcxDecodeError("not MT44")
    camf = decode_camf(frame.raw)
    extended = decode_extended(frame.raw, camf)
    if receipt_time is None:
        receipt_time = datetime.now(timezone.utc)
    try:
        report = HexQzssDcrDecoder(
            sentence=frame.raw.hex().upper()[:63], timestamp=receipt_time
        ).decode()
        fields: dict[str, Any] = json_safe(report.get_params())
        fields.pop("message", None)
        fields.pop("raw", None)
    except (QzssDcrDecoderException, QzssDcrDecoderNotImplementedError,
            EOFError) as exc:
        raise DcxDecodeError(str(exc)) from exc
    payload = DcxPayload(camf, extended,
                         tuple(sorted(fields.items(), key=lambda item: item[0])))
    action = {0: "issue", 1: "issue", 2: "update", 3: "clear"}.get(
        camf.a1_message_type, "unknown")
    status = "test" if camf.a1_message_type == 0 else "actual"
    if fields.get("dcx_message_type") == "Null Message":
        status, action = "not_applicable", "none"
    return payload, hazard(camf.a4_hazard), status, action, fields, \
        areas_from_extended(extended)
