#!/usr/bin/env python3
"""コマンドラインツール共通の、バイナリを壊さない捕捉入力。"""

from __future__ import annotations

import hashlib
import pathlib
import re
from dataclasses import dataclass


TRUSTED_RECORD_RE = re.compile(
    rb"(?:HoshiGuard v|META |META_PRIVACY |GNSS_STATE |GNSS_TX |"
    rb"UBX_RAW timestamp_us=|QZSS_L1S_UBX timestamp_us=|"
    rb"PPS timestamp_us=|TIMTP timestamp_us=|TIMTP_MATCH timestamp_us=|"
    rb"SFRBX_HDR timestamp_us=|SFRBX timestamp_us=|L1S timestamp_us=|STATUS )"
)


@dataclass(frozen=True)
class CaptureInput:
    path: pathlib.Path
    size_bytes: int
    sha256: str
    first_record_offset: int
    leading_garbage_bytes: int
    leading_summary_hex: str
    lines: list[str]
    warnings: list[str]


def _printable_record(candidate: bytes) -> bool:
    if not candidate or len(candidate) > 4096:
        return False
    return all(value in (9, 13) or 32 <= value <= 126 for value in candidate)


def find_first_record_offset(data: bytes) -> int:
    """構造が妥当な最初の HoshiGuard レコードの offset を返す。"""
    for match in TRUSTED_RECORD_RE.finditer(data):
        end = data.find(b"\n", match.start())
        if end < 0:
            end = len(data)
        if _printable_record(data[match.start():end].rstrip(b"\r")):
            return match.start()
    return len(data)


def read_capture(path: pathlib.Path) -> CaptureInput:
    data = path.read_bytes()
    offset = find_first_record_offset(data)
    warnings: list[str] = []
    lines: list[str] = []
    if offset:
        warnings.append(f"skipped {offset} leading garbage bytes")
    for number, raw in enumerate(data[offset:].splitlines(), 1):
        try:
            line = raw.decode("ascii")
        except UnicodeDecodeError:
            warnings.append(f"record {number}: non-ASCII bytes skipped")
            continue
        if not _printable_record(raw.rstrip(b"\r")):
            warnings.append(f"record {number}: control bytes skipped")
            continue
        lines.append(line.rstrip("\r"))
    return CaptureInput(
        path=path,
        size_bytes=len(data),
        sha256=hashlib.sha256(data).hexdigest(),
        first_record_offset=offset,
        leading_garbage_bytes=offset,
        leading_summary_hex=data[: min(offset, 16)].hex().upper(),
        lines=lines,
        warnings=warnings,
    )
