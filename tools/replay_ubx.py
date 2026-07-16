#!/usr/bin/env python3
"""Replay HoshiGuard UBX_RAW logs with the firmware's L1S rules.

Protocol constants mirror include/qzss_l1s.h and src/qzss_l1s.cpp:
250 valid MSB-first bits, MT at bit 8 for 6 bits, CRC data bits 0..225,
CRC bits 226..249, polynomial low 24 bits 0x864CFB, init/final xor zero.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from dataclasses import dataclass
from typing import Iterable, Optional

import capture_io

UBX_RE = re.compile(
    r"^(UBX_RAW|QZSS_L1S_UBX) timestamp_us=(\d+).*?\blen=(\d+) "
    r"data=([0-9A-Fa-f]+)(?:\s|$)"
)
UBX_SYNC = b"\xB5\x62"
RXM_SFRBX_CLASS = 0x02
RXM_SFRBX_ID = 0x13
QZSS_GNSS_ID = 5
QZSS_L1S_SIG_ID = 1
L1S_BYTES = 32
L1S_VALID_BITS = 250
L1S_CRC_DATA_BITS = 226
L1S_CRC_POLYNOMIAL = 0x864CFB
DEDUP_WINDOW_US = 10_000_000
VALID_PAB = frozenset((0x53, 0x9A, 0xC6))


@dataclass(frozen=True)
class UbxRecord:
    timestamp_us: int
    packet: bytes
    line_number: int
    record_type: str = "UBX_RAW"


@dataclass(frozen=True)
class SfrbxRecord:
    gnss_id: int
    sv_id: int
    sig_id: int
    freq_id: int
    num_words: int
    payload: bytes


@dataclass(frozen=True)
class L1sRecord:
    timestamp_us: int
    sv_id: int
    raw: bytes
    pab: int
    message_type: int
    valid_pab: bool
    valid_crc: bool
    duplicate: bool
    received_crc: int
    calculated_crc: int
    num_words: int
    extra_words: bytes


def ubx_checksum(data: bytes) -> tuple[int, int]:
    ck_a = 0
    ck_b = 0
    for value in data:
        ck_a = (ck_a + value) & 0xFF
        ck_b = (ck_b + ck_a) & 0xFF
    return ck_a, ck_b


def validate_ubx(packet: bytes) -> Optional[str]:
    if len(packet) < 8:
        return "packet shorter than 8 bytes"
    if packet[:2] != UBX_SYNC:
        return "invalid sync"
    payload_length = packet[4] | (packet[5] << 8)
    if len(packet) != payload_length + 8:
        return f"length mismatch header={payload_length} actual={len(packet) - 8}"
    expected = ubx_checksum(packet[2:-2])
    received = (packet[-2], packet[-1])
    if received != expected:
        return f"checksum mismatch received={received[0]:02X}{received[1]:02X} expected={expected[0]:02X}{expected[1]:02X}"
    return None


def parse_log(lines: Iterable[str]) -> tuple[list[UbxRecord], list[str]]:
    records: list[UbxRecord] = []
    errors: list[str] = []
    for line_number, line in enumerate(lines, 1):
        if "UBX_RAW" not in line and "QZSS_L1S_UBX" not in line:
            continue
        match = UBX_RE.search(line)
        if match is None:
            errors.append(f"line {line_number}: malformed raw UBX record")
            continue
        declared = int(match.group(3))
        hex_data = match.group(4)
        if len(hex_data) % 2:
            errors.append(f"line {line_number}: odd number of hex digits")
            continue
        try:
            packet = bytes.fromhex(hex_data)
        except ValueError as exc:
            errors.append(f"line {line_number}: invalid hex: {exc}")
            continue
        if len(packet) != declared:
            errors.append(
                f"line {line_number}: record len={declared} but hex has {len(packet)} bytes"
            )
            continue
        error = validate_ubx(packet)
        if error:
            errors.append(f"line {line_number}: {error}")
            continue
        records.append(UbxRecord(int(match.group(2)), packet, line_number,
                                 match.group(1)))
    return records, errors


def parse_sfrbx(packet: bytes) -> Optional[SfrbxRecord]:
    if len(packet) < 16 or packet[2:4] != bytes((RXM_SFRBX_CLASS,
                                                 RXM_SFRBX_ID)):
        return None
    payload = packet[6:-2]
    if len(payload) < 8:
        return None
    gnss_id, sv_id, sig_id, freq_id, num_words = payload[:5]
    if num_words > 16 or len(payload) != 8 + 4 * num_words:
        return None
    return SfrbxRecord(gnss_id, sv_id, sig_id, freq_id, num_words, payload)


def read_bits_msb(data: bytes, offset: int, count: int) -> int:
    if count < 0 or count > 32 or offset < 0 or offset + count > L1S_VALID_BITS:
        raise ValueError("bit read outside 250-bit L1S message")
    value = 0
    for bit in range(offset, offset + count):
        value = (value << 1) | ((data[bit >> 3] >> (7 - (bit & 7))) & 1)
    return value


def l1s_crc24(raw: bytes) -> int:
    if len(raw) != L1S_BYTES:
        raise ValueError("L1S frame must be exactly 32 bytes")
    crc = 0
    for bit in range(L1S_CRC_DATA_BITS):
        input_bit = (raw[bit >> 3] >> (7 - (bit & 7))) & 1
        feedback = ((crc >> 23) & 1) ^ input_bit
        crc = (crc << 1) & 0xFFFFFF
        if feedback:
            crc ^= L1S_CRC_POLYNOMIAL
    return crc


def extract_l1s(record: UbxRecord, seen: dict[bytes, int]) -> Optional[L1sRecord]:
    header = parse_sfrbx(record.packet)
    if header is None:
        return None
    if (header.gnss_id != QZSS_GNSS_ID or
            header.sig_id != QZSS_L1S_SIG_ID or
            header.num_words not in (8, 9)):
        return None
    payload = header.payload
    # UBX U4 is little-endian; the numeric word's bit 31 is transmitted first.
    raw = b"".join(payload[8 + word * 4 : 12 + word * 4][::-1] for word in range(8))
    pab = raw[0]
    message_type = read_bits_msb(raw, 8, 6)
    received_crc = read_bits_msb(raw, L1S_CRC_DATA_BITS, 24)
    canonical = raw[:31] + bytes((raw[31] & 0xC0,))
    previous = seen.get(canonical)
    duplicate = (
        previous is not None
        and record.timestamp_us >= previous
        and record.timestamp_us - previous <= DEDUP_WINDOW_US
    )
    seen[canonical] = record.timestamp_us
    calculated_crc = l1s_crc24(raw)
    return L1sRecord(
        record.timestamp_us,
        header.sv_id,
        raw,
        pab,
        message_type,
        pab in VALID_PAB,
        calculated_crc == received_crc,
        duplicate,
        received_crc,
        calculated_crc,
        header.num_words,
        payload[40:8 + 4 * header.num_words],
    )


def export_cpp(path: pathlib.Path, records: list[UbxRecord]) -> None:
    guard = "HOSHIGUARD_CAPTURED_VECTORS_H"
    with path.open("w", encoding="utf-8", newline="\n") as output:
        output.write(f"#ifndef {guard}\n#define {guard}\n\n#include <stddef.h>\n#include <stdint.h>\n\n")
        for index, record in enumerate(records):
            values = ", ".join(f"0x{value:02X}" for value in record.packet)
            output.write(f"static const uint8_t kCapturedUbx{index}[] = {{{values}}};\n")
        output.write("\nstruct CapturedUbxVector { const uint8_t *data; size_t length; };\n")
        if records:
            output.write("static const CapturedUbxVector kCapturedUbxVectors[] = {\n")
            for index in range(len(records)):
                output.write(f"    {{kCapturedUbx{index}, sizeof(kCapturedUbx{index})}},\n")
            output.write("};\n")
            output.write(
                "static const size_t kCapturedUbxVectorCount = "
                "sizeof(kCapturedUbxVectors) / sizeof(kCapturedUbxVectors[0]);\n\n"
            )
        else:
            output.write("static const CapturedUbxVector kCapturedUbxVectors[1] = {{0, 0}};\n")
            output.write("static const size_t kCapturedUbxVectorCount = 0;\n\n")
        output.write(f"#endif  // {guard}\n")


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("--only-l1s", action="store_true")
    parser.add_argument("--export-cpp", type=pathlib.Path)
    parser.add_argument(
        "--export-valid-l1s-cpp", type=pathlib.Path,
        help="export only checksum-valid QZSS L1S RXM-SFRBX packets",
    )
    args = parser.parse_args(argv)

    try:
        capture = capture_io.read_capture(args.capture)
        records, errors = parse_log(capture.lines)
    except OSError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    for warning in capture.warnings:
        print(f"warning: {warning}", file=sys.stderr)
    for error in errors:
        print(error, file=sys.stderr)
    if args.export_cpp is not None:
        try:
            export_cpp(args.export_cpp, records)
        except OSError as exc:
            print(f"error: cannot export C++ vectors: {exc}", file=sys.stderr)
            return 2
    if args.export_valid_l1s_cpp is not None:
        export_seen: dict[bytes, int] = {}
        selected = []
        for record in records:
            frame = extract_l1s(record, export_seen)
            if frame is not None and frame.valid_pab and frame.valid_crc:
                selected.append(record)
        try:
            export_cpp(args.export_valid_l1s_cpp, selected)
        except OSError as exc:
            print(f"error: cannot export valid L1S vectors: {exc}", file=sys.stderr)
            return 2

    seen: dict[bytes, int] = {}
    sfrbx_count = 0
    qzss_count = 0
    l1s_records: list[L1sRecord] = []
    for record in records:
        packet = record.packet
        if packet[2] == RXM_SFRBX_CLASS and packet[3] == RXM_SFRBX_ID:
            sfrbx_count += 1
            header = parse_sfrbx(packet)
            payload = packet[6:-2]
            if header is not None and header.gnss_id == QZSS_GNSS_ID:
                qzss_count += 1
            if not args.only_l1s and header is not None:
                print(
                    f"SFRBX line={record.line_number} timestamp_us={record.timestamp_us} "
                    f"gnssId={header.gnss_id} svId={header.sv_id} sigId={header.sig_id} "
                    f"freqId={header.freq_id} numWords={header.num_words}"
                )
        l1s = extract_l1s(record, seen)
        if l1s is None:
            continue
        l1s_records.append(l1s)
        name = "DC Report" if l1s.message_type == 43 else "DCX" if l1s.message_type == 44 else "unsupported"
        print(
            f"L1S timestamp_us={l1s.timestamp_us} svId={l1s.sv_id} "
            f"pab={l1s.pab:02X} mt={l1s.message_type} name={name} "
            f"pab_ok={'yes' if l1s.valid_pab else 'no'} "
            f"crc={'ok' if l1s.valid_crc else 'bad'} "
            f"received_crc={l1s.received_crc:06X} "
            f"calculated_crc={l1s.calculated_crc:06X} "
            f"duplicate={'yes' if l1s.duplicate else 'no'} numWords={l1s.num_words} "
            f"extra_words={l1s.extra_words.hex().upper()} raw={l1s.raw.hex().upper()}"
        )

    print(
        f"SUMMARY ubx_valid={len(records)} malformed={len(errors)} "
        f"sfrbx={sfrbx_count} qzss_sfrbx={qzss_count} "
        f"qzss_l1s={len(l1s_records)} valid_crc={sum(r.valid_crc for r in l1s_records)} "
        f"duplicates={sum(r.duplicate for r in l1s_records)}"
    )
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
