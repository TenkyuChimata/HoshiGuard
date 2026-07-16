"""HoshiGuard Bridge v1 framing.

Wire records are COBS(logical-frame) followed by 0x00.  The logical frame
ends with little-endian CRC-32/ISO-HDLC (poly=0x04C11DB7, init=0xffffffff,
refin/refout=true, xorout=0xffffffff; reflected poly=0xedb88320).
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum
import struct
import zlib

MAGIC = b"HG"
PROTOCOL_MAJOR = 1
PROTOCOL_MINOR = 0
HEADER = struct.Struct("<2sBBBBHHHIQ")
HEADER_LENGTH = HEADER.size  # 24
MAX_LOGICAL_FRAME = 384
MAX_ENCODED_FRAME = MAX_LOGICAL_FRAME + MAX_LOGICAL_FRAME // 254 + 2


class BridgeError(ValueError):
    pass


class CobsError(BridgeError):
    pass


class CrcError(BridgeError):
    pass


class VersionError(BridgeError):
    pass


class FrameType(IntEnum):
    HELLO = 1
    L1S_FRAME = 2
    STATUS = 3
    TIME_STATUS = 4
    ERROR = 5
    HEARTBEAT = 6


def crc32_iso_hdlc(data: bytes) -> int:
    """Return CRC-32/ISO-HDLC; check value for b'123456789' is CBF43926."""
    return zlib.crc32(data) & 0xFFFFFFFF


def cobs_encode(data: bytes) -> bytes:
    output = bytearray((0,))
    code_index = 0
    code = 1
    for byte in data:
        if byte == 0:
            output[code_index] = code
            code_index = len(output)
            output.append(0)
            code = 1
        else:
            output.append(byte)
            code += 1
            if code == 0xFF:
                output[code_index] = code
                code_index = len(output)
                output.append(0)
                code = 1
    output[code_index] = code
    return bytes(output)


def cobs_decode(data: bytes) -> bytes:
    if not data:
        raise CobsError("empty COBS record")
    output = bytearray()
    index = 0
    while index < len(data):
        code = data[index]
        if code == 0:
            raise CobsError("zero inside COBS record")
        index += 1
        end = index + code - 1
        if end > len(data):
            raise CobsError("COBS code exceeds record")
        block = data[index:end]
        if 0 in block:
            raise CobsError("encoded COBS block contains zero")
        output.extend(block)
        index = end
        if code != 0xFF and index < len(data):
            output.append(0)
    return bytes(output)


@dataclass(frozen=True)
class BridgeFrame:
    major: int
    minor: int
    frame_type: int
    flags: int
    sequence: int
    timestamp_us: int
    payload: bytes
    crc32: int
    wire: bytes = b""

    def logical_bytes(self) -> bytes:
        header = HEADER.pack(
            MAGIC, self.major, self.minor, self.frame_type, 0,
            HEADER_LENGTH, len(self.payload), self.flags,
            self.sequence, self.timestamp_us,
        )
        body = header + self.payload
        return body + struct.pack("<I", crc32_iso_hdlc(body))

    def wire_bytes(self) -> bytes:
        return cobs_encode(self.logical_bytes()) + b"\0"


def encode_frame(frame_type: int, sequence: int, timestamp_us: int,
                 payload: bytes = b"", flags: int = 0,
                 minor: int = PROTOCOL_MINOR) -> bytes:
    return BridgeFrame(PROTOCOL_MAJOR, minor, frame_type, flags, sequence,
                       timestamp_us, payload, 0).wire_bytes()


def decode_logical(data: bytes, supported_major: int = PROTOCOL_MAJOR) -> BridgeFrame:
    if len(data) < HEADER_LENGTH + 4 or len(data) > MAX_LOGICAL_FRAME:
        raise BridgeError("invalid logical frame length")
    magic, major, minor, frame_type, reserved, header_length, payload_length, \
        flags, sequence, timestamp_us = HEADER.unpack_from(data)
    if magic != MAGIC or reserved != 0 or header_length != HEADER_LENGTH:
        raise BridgeError("invalid bridge header")
    if major != supported_major:
        raise VersionError(f"unsupported bridge major {major}")
    expected = HEADER_LENGTH + payload_length + 4
    if len(data) != expected:
        raise BridgeError("payload length mismatch")
    received_crc, = struct.unpack_from("<I", data, len(data) - 4)
    calculated_crc = crc32_iso_hdlc(data[:-4])
    if received_crc != calculated_crc:
        raise CrcError(
            f"bridge CRC mismatch received={received_crc:08X} "
            f"calculated={calculated_crc:08X}"
        )
    return BridgeFrame(major, minor, frame_type, flags, sequence,
                       timestamp_us, data[HEADER_LENGTH:-4], received_crc)


def decode_wire(record: bytes) -> BridgeFrame:
    encoded = record[:-1] if record.endswith(b"\0") else record
    frame = decode_logical(cobs_decode(encoded))
    return BridgeFrame(**{**frame.__dict__, "wire": encoded + b"\0"})


@dataclass
class StreamStats:
    bytes_received: int = 0
    frames: int = 0
    cobs_errors: int = 0
    crc_errors: int = 0
    version_errors: int = 0
    oversized: int = 0
    empty_delimiters: int = 0
    partial_discarded: int = 0


class StreamDecoder:
    """Delimiter-resynchronizing decoder for arbitrarily fragmented reads."""

    def __init__(self, maximum: int = MAX_ENCODED_FRAME):
        self.maximum = maximum
        self.buffer = bytearray()
        self.discarding = False
        self.stats = StreamStats()

    def feed(self, data: bytes) -> list[BridgeFrame]:
        frames: list[BridgeFrame] = []
        self.stats.bytes_received += len(data)
        for byte in data:
            if byte == 0:
                if self.discarding:
                    self.discarding = False
                    self.buffer.clear()
                    continue
                if not self.buffer:
                    self.stats.empty_delimiters += 1
                    continue
                record = bytes(self.buffer)
                self.buffer.clear()
                try:
                    frame = decode_wire(record + b"\0")
                except CrcError:
                    self.stats.crc_errors += 1
                except VersionError:
                    self.stats.version_errors += 1
                except BridgeError:
                    self.stats.cobs_errors += 1
                else:
                    self.stats.frames += 1
                    frames.append(frame)
            elif not self.discarding:
                if len(self.buffer) >= self.maximum:
                    self.buffer.clear()
                    self.discarding = True
                    self.stats.oversized += 1
                else:
                    self.buffer.append(byte)
        return frames

    def discard_partial(self) -> None:
        if self.buffer or self.discarding:
            self.stats.partial_discarded += 1
        self.buffer.clear()
        self.discarding = False


class SequenceDisposition(IntEnum):
    FIRST = 0
    CONTIGUOUS = 1
    GAP = 2
    DUPLICATE = 3
    OUT_OF_ORDER = 4
    NEW_EPOCH = 5


@dataclass(frozen=True)
class SequenceResult:
    disposition: SequenceDisposition
    missing: int = 0
    epoch: int = 0


class SequenceTracker:
    def __init__(self) -> None:
        self.last: int | None = None
        self.epoch = 0
        self.gaps = 0
        self.duplicates = 0
        self.out_of_order = 0

    def observe(self, sequence: int, hello: bool = False) -> SequenceResult:
        sequence &= 0xFFFFFFFF
        if hello and self.last is not None and sequence <= self.last:
            self.epoch += 1
            self.last = sequence
            return SequenceResult(SequenceDisposition.NEW_EPOCH, epoch=self.epoch)
        if self.last is None:
            self.last = sequence
            return SequenceResult(SequenceDisposition.FIRST, epoch=self.epoch)
        expected = (self.last + 1) & 0xFFFFFFFF
        if sequence == self.last:
            self.duplicates += 1
            return SequenceResult(SequenceDisposition.DUPLICATE, epoch=self.epoch)
        if sequence == expected:
            self.last = sequence
            return SequenceResult(SequenceDisposition.CONTIGUOUS, epoch=self.epoch)
        forward = (sequence - expected) & 0xFFFFFFFF
        if forward < 0x80000000:
            self.gaps += forward
            self.last = sequence
            return SequenceResult(SequenceDisposition.GAP, forward, self.epoch)
        self.out_of_order += 1
        return SequenceResult(SequenceDisposition.OUT_OF_ORDER, epoch=self.epoch)
