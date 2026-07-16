"""Small independent UBX verifier used at the PC trust boundary."""

from __future__ import annotations

from dataclasses import dataclass
import struct


class UbxError(ValueError):
    pass


@dataclass(frozen=True)
class UbxPacket:
    message_class: int
    message_id: int
    payload: bytes
    checksum_a: int
    checksum_b: int
    raw: bytes


def checksum(data: bytes) -> tuple[int, int]:
    a = b = 0
    for byte in data:
        a = (a + byte) & 0xFF
        b = (b + a) & 0xFF
    return a, b


def parse_packet(raw: bytes) -> UbxPacket:
    if len(raw) < 8 or raw[:2] != b"\xB5\x62":
        raise UbxError("missing UBX sync or packet too short")
    payload_length, = struct.unpack_from("<H", raw, 4)
    if len(raw) != payload_length + 8:
        raise UbxError("UBX payload length mismatch")
    expected = checksum(raw[2:-2])
    received = (raw[-2], raw[-1])
    if expected != received:
        raise UbxError(
            f"UBX checksum mismatch received={received[0]:02X}{received[1]:02X} "
            f"calculated={expected[0]:02X}{expected[1]:02X}"
        )
    return UbxPacket(raw[2], raw[3], raw[6:-2], *received, raw)


@dataclass(frozen=True)
class Sfrbx:
    gnss_id: int
    sv_id: int
    sig_id: int
    freq_id: int
    num_words: int
    channel: int
    version: int
    words_ubx: tuple[bytes, ...]
    packet: UbxPacket


def parse_sfrbx(packet: UbxPacket) -> Sfrbx:
    if (packet.message_class, packet.message_id) != (0x02, 0x13):
        raise UbxError("not UBX-RXM-SFRBX")
    if len(packet.payload) < 8:
        raise UbxError("SFRBX header too short")
    num_words = packet.payload[4]
    if num_words > 16 or len(packet.payload) != 8 + num_words * 4:
        raise UbxError("SFRBX numWords/payload length mismatch")
    words = tuple(packet.payload[8 + i * 4:12 + i * 4]
                  for i in range(num_words))
    return Sfrbx(packet.payload[0], packet.payload[1], packet.payload[2],
                 packet.payload[3], num_words, packet.payload[5],
                 packet.payload[6], words, packet)

