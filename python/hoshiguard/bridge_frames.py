"""Typed Bridge v1 payloads."""

from __future__ import annotations

from dataclasses import dataclass
import struct

from .bridge_protocol import BridgeError, BridgeFrame, FrameType


@dataclass(frozen=True)
class Hello:
    protocol_major: int
    protocol_minor: int
    firmware: str
    privacy_mode: bool
    little_endian: bool
    crc_identifier: int
    feature_flags: int
    debug_baud: int
    gnss_baud: int
    maximum_ubx: int
    maximum_logical: int
    board: str
    build: str


@dataclass(frozen=True)
class L1sFrame:
    pps_sequence: int
    sv_id: int
    gnss_id: int
    sig_id: int
    num_words: int
    pab: int
    message_type: int
    claimed_flags: int
    ubx: bytes
    time_quality: int
    pps_state: int
    firmware: str


@dataclass(frozen=True)
class Status:
    uptime_ms: int
    current_baud: int
    gnss_state: int
    pps_state: int
    time_quality: int
    last_error: int
    ubx_received: int
    ubx_checksum_errors: int
    sfrbx_received: int
    qzss_l1s_received: int
    valid_l1s: int
    bridge_generated: int
    bridge_queued: int
    bridge_sent: int
    bridge_dropped: int
    critical_dropped: int
    uart_output_bytes: int
    queue_high_watermark: int
    pps_edges: int
    pps_lost_total: int
    pps_abnormal_total: int
    l1s_generated: int
    l1s_sent: int
    l1s_dropped: int
    encode_errors: int


@dataclass(frozen=True)
class TimeStatus:
    pps_sequence: int
    pps_state: int
    quality: int
    reason: int
    tim_tp_flags: int
    q_err_ps: int | None
    ref_info: int
    week: int
    tow_ms: int
    tow_sub_ms: int
    matched_pps_sequence: int
    matched: bool
    ambiguous: bool
    time_grid_known: bool


@dataclass(frozen=True)
class ErrorFrame:
    code: int
    evidence0: int
    evidence1: int


def _ascii(value: bytes) -> str:
    return value.split(b"\0", 1)[0].decode("ascii", "replace")


def parse_payload(frame: BridgeFrame):
    payload = frame.payload
    if frame.frame_type == FrameType.HELLO:
        if len(payload) != 64:
            raise BridgeError("HELLO payload must be 64 bytes")
        feature, debug_baud, gnss_baud, max_ubx, max_logical = struct.unpack_from(
            "<IIIHH", payload, 8)
        return Hello(payload[0], payload[1], f"{payload[2]}.{payload[3]}.{payload[4]}",
                     bool(payload[5]), payload[6] == 1, payload[7], feature,
                     debug_baud, gnss_baud, max_ubx, max_logical,
                     _ascii(payload[24:40]), _ascii(payload[40:64]))
    if frame.frame_type == FrameType.L1S_FRAME:
        if len(payload) < 20:
            raise BridgeError("L1S payload shorter than metadata")
        pps, = struct.unpack_from("<I", payload)
        flags, ubx_length = struct.unpack_from("<HH", payload, 10)
        if len(payload) != 20 + ubx_length:
            raise BridgeError("L1S UBX length mismatch")
        return L1sFrame(pps, payload[4], payload[5], payload[6], payload[7],
                        payload[8], payload[9], flags, payload[20:], payload[14],
                        payload[15], f"{payload[16]}.{payload[17]}.{payload[18]}")
    if frame.frame_type == FrameType.STATUS:
        if len(payload) != 88:
            raise BridgeError("STATUS payload must be 88 bytes")
        uptime, baud = struct.unpack_from("<II", payload)
        counters = struct.unpack_from("<19I", payload, 12)
        return Status(uptime, baud, payload[8], payload[9], payload[10],
                      payload[11], *counters)
    if frame.frame_type == FrameType.TIME_STATUS:
        if len(payload) != 32:
            raise BridgeError("TIME_STATUS payload must be 32 bytes")
        pps, = struct.unpack_from("<I", payload)
        q_err, = struct.unpack_from("<i", payload, 8)
        week, = struct.unpack_from("<H", payload, 14)
        tow_ms, tow_sub_ms, matched_pps = struct.unpack_from("<III", payload, 16)
        match_flags = payload[28]
        return TimeStatus(pps, payload[4], payload[5], payload[6], payload[7],
                          q_err if payload[12] else None, payload[13], week,
                          tow_ms, tow_sub_ms, matched_pps,
                          bool(match_flags & 1), bool(match_flags & 2),
                          bool(match_flags & 4))
    if frame.frame_type == FrameType.ERROR:
        if len(payload) != 12:
            raise BridgeError("ERROR payload must be 12 bytes")
        code, _reserved, evidence0, evidence1 = struct.unpack("<HHII", payload)
        return ErrorFrame(code, evidence0, evidence1)
    if frame.frame_type == FrameType.HEARTBEAT:
        if len(payload) != 4:
            raise BridgeError("HEARTBEAT payload must be 4 bytes")
        return struct.unpack("<I", payload)[0]
    return payload
