"""Route independently verified L1S frames to service decoders."""

from __future__ import annotations

from datetime import datetime, timezone

from .bridge_frames import L1sFrame as ClaimedL1sFrame
from .bridge_protocol import BridgeFrame
from .dcr.decoder import decode_dcr
from .dcr.reassembly import NankaiTroughReassembler
from .dcx.decoder import decode_dcx
from .l1s import VerifiedL1s
from .events.canonical import SEMANTIC_FINGERPRINT_VERSION, semantic_fingerprint
from .models import (
    AlertCode, AlertIdentity, AlertReceipt, AlertService, AlertTimes,
    AlertValidity, RawEvidence, UnifiedAlert, UnknownField, stable_digest,
)


def service_for_mt(mt: int) -> tuple[AlertService, bool, str]:
    if mt == 0:
        return AlertService.TEST, False, "IS-QZSS-L1S-009"
    if mt in (40, 41):
        return AlertService.RESERVED, False, "IS-QZSS-L1S-009"
    if mt == 43:
        return AlertService.DCR, True, "IS-QZSS-DCR-016"
    if mt == 44:
        return AlertService.DCX, True, "IS-QZSS-DCX-004/CAMF-1.2"
    if mt in (47, 48, 49, 50, 51):
        return AlertService.SLAS, False, "IS-QZSS-L1S-009"
    if mt == 63:
        return AlertService.NULL, False, "IS-QZSS-L1S-009"
    return AlertService.UNKNOWN, False, "IS-QZSS-L1S-009"


def _event_key(service: AlertService, status: str, category: AlertCode,
               payload) -> str:
    # Actual and test messages occupy separate event spaces. The key remains
    # explainable; confidence is limited when the normative fields do not
    # provide a globally unique event identifier.
    identity_parts = [service.value, status, str(category.code)]
    if service == AlertService.DCR and hasattr(payload, "fields"):
        fields = dict(payload.fields)
        identity_parts.extend((str(fields.get("report_time", "unknown")),
                               str(fields.get("information_serial_code_raw",
                                              "none"))))
    elif service == AlertService.DCX and hasattr(payload, "camf"):
        camf = payload.camf
        identity_parts.extend((str(camf.a2_country_region),
                               str(camf.a3_provider),
                               str(camf.a6_onset_week),
                               str(camf.a7_onset_minute_of_week)))
        identity_parts.extend(str(value) for name, value in payload.extended.raw_fields
                              if name in ("EX1_target_area_code",
                                          "EX9_target_area_code_list"))
    return ":".join(identity_parts)


def make_alert(bridge: BridgeFrame, claim: ClaimedL1sFrame,
               frame: VerifiedL1s, bridge_crc: bool = True,
               dcr_reassembler: NankaiTroughReassembler | None = None
               ) -> UnifiedAlert:
    service, is_alert, spec = service_for_mt(frame.message_type)
    receipt = AlertReceipt(bridge.sequence, bridge.timestamp_us,
                           claim.pps_sequence, frame.sfrbx.sv_id, frame.pab,
                           frame.message_type)
    raw = RawEvidence(frame.raw.hex().upper(), frame.sfrbx.packet.raw.hex().upper(),
                      f"{frame.received_crc:06X}",
                      f"{frame.calculated_crc:06X}", frame.spare_bits,
                      frame.extra_word_ubx.hex().upper()
                      if frame.extra_word_ubx else None)
    unknown: list[UnknownField] = []
    semantics = True
    complete = True
    areas = ()
    severity = None
    times = AlertTimes()
    status = "unknown"
    action = "none"
    category = AlertCode(frame.message_type, f"MT{frame.message_type}", True)
    payload = {"raw_payload_hex": frame.raw.hex().upper()[:57]}
    if service == AlertService.DCR:
        payload, category, status, action, fields = decode_dcr(
            frame, reassembler=dcr_reassembler)
        report_time = fields.get("report_time")
        times = AlertTimes(issued=str(report_time) if report_time else None)
        if category.code == 4 and not fields.get("reassembly_complete", False):
            complete = False
            unknown.append(UnknownField(
                "dcr_continuation", {
                    "page": fields.get("page_number"),
                    "total": fields.get("total_page")},
                reason="awaiting_bounded_nankai_reassembly"))
    elif service == AlertService.DCX:
        payload, category, status, action, fields, areas = decode_dcx(frame)
        severity = {"code": payload.camf.a5_severity,
                    "raw": payload.camf.a5_severity}
        if payload.extended.blocked_by_missing_spec:
            complete = False
            for reason in payload.extended.blocked_by_missing_spec:
                unknown.append(UnknownField("EX11", dict(payload.extended.raw_fields),
                                            146, 68, reason))
        if category.code == 0 and status == "not_applicable":
            is_alert = False
            severity = None
            areas = ()
            category = AlertCode(0, "NULL_MESSAGE", True, True,
                                 official_en="Null message")
    elif service == AlertService.TEST:
        status, action = "test", "none"
    elif service == AlertService.NULL:
        status, action = "not_applicable", "none"
    elif service in (AlertService.SLAS, AlertService.RESERVED,
                     AlertService.UNKNOWN):
        status, action = "not_alert", "none"
        if service in (AlertService.RESERVED, AlertService.UNKNOWN):
            unknown.append(UnknownField("message_type", frame.message_type,
                                        8, 6, "reserved_or_unknown_mt"))
    validity = AlertValidity(bridge_crc, True, frame.valid_crc,
                             frame.valid_pab, semantics, complete)
    # First build the immutable semantic model without identity.  Identity is
    # then derived from that model so receipt/raw transport evidence can never
    # leak into the v2 semantic fingerprint.
    placeholder = AlertIdentity("", "", SEMANTIC_FINGERPRINT_VERSION,
                                _event_key(service, status, category, payload),
                                "limited")
    provisional = UnifiedAlert("hoshiguard.alert.v1", "QZSS_L1S", service, spec,
                        receipt, validity, status, action, placeholder, category,
                        severity, times, areas, payload, tuple(unknown), raw,
                        alert=is_alert)
    frame_hash = stable_digest({
        "ubx": frame.sfrbx.packet.raw.hex().upper(),
        "pab": frame.pab,
        "mt": frame.message_type,
    })
    identity = AlertIdentity(frame_hash, semantic_fingerprint(provisional),
                             SEMANTIC_FINGERPRINT_VERSION,
                             placeholder.event_key, "limited")
    return UnifiedAlert(**{**provisional.__dict__, "identity": identity})
