"""IS-QZSS-L1S-009 に基づく独立した伝送検証。"""

from __future__ import annotations

from dataclasses import dataclass

from .bridge_frames import L1sFrame as ClaimedL1sFrame
from .ubx import Sfrbx, UbxError, parse_packet, parse_sfrbx

VALID_PABS = (0x53, 0x9A, 0xC6)
VALID_BITS = 250
CRC_DATA_BITS = 226
CRC_POLYNOMIAL = 0x864CFB
SUPPORTED_WORD_COUNTS = (8, 9)

FLAG_UBX_VALID = 1 << 0
FLAG_LENGTH_VALID = 1 << 1
FLAG_PAB_VALID = 1 << 2
FLAG_CRC_VALID = 1 << 3


class L1sError(ValueError):
    pass


def read_bits(data: bytes, offset: int, width: int, bit_length: int = VALID_BITS) -> int:
    """指定 bit 長の範囲内で、MSB-first の符号なしフィールドを読む。

    Python 整数は任意精度であり、幅の制限は ``bit_length`` と入力の長さに従い、マシン word 長には依存しない。
    """
    if not all(isinstance(value, int) for value in
               (offset, width, bit_length)):
        raise L1sError("bit offset, width and length must be integers")
    storage_bits = len(data) * 8
    if offset < 0 or width < 0 or bit_length < 0:
        raise L1sError("bit field outside valid L1S range")
    if bit_length > storage_bits:
        raise L1sError("bit length exceeds supplied data")
    if offset > bit_length or width > bit_length - offset:
        raise L1sError("bit field outside valid L1S range")
    value = 0
    for bit in range(offset, offset + width):
        value = (value << 1) | ((data[bit >> 3] >> (7 - (bit & 7))) & 1)
    return value


def crc24(data: bytes) -> int:
    if len(data) != 32:
        raise L1sError("L1S container must be 32 bytes")
    crc = 0
    for index in range(CRC_DATA_BITS):
        input_bit = (data[index >> 3] >> (7 - (index & 7))) & 1
        feedback = ((crc >> 23) & 1) ^ input_bit
        crc = (crc << 1) & 0xFFFFFF
        if feedback:
            crc ^= CRC_POLYNOMIAL
    return crc


@dataclass(frozen=True)
class VerifiedL1s:
    sfrbx: Sfrbx
    raw: bytes
    extra_word_ubx: bytes | None
    pab: int
    message_type: int
    received_crc: int
    calculated_crc: int
    spare_bits: int

    @property
    def valid_pab(self) -> bool:
        return self.pab in VALID_PABS

    @property
    def valid_crc(self) -> bool:
        return self.received_crc == self.calculated_crc


def verify_ubx_l1s(raw_ubx: bytes) -> VerifiedL1s:
    try:
        sfrbx = parse_sfrbx(parse_packet(raw_ubx))
    except UbxError as exc:
        raise L1sError(str(exc)) from exc
    if sfrbx.gnss_id != 5 or sfrbx.sig_id != 1:
        raise L1sError("SFRBX is not QZSS L1S (gnssId=5, sigId=1)")
    if sfrbx.num_words not in SUPPORTED_WORD_COUNTS:
        raise L1sError("unsupported QZSS L1S numWords")
    # UBX の各 U4 はリトルエンディアンで、bit 31 が L1S の最初の送信 bit。先頭 8 word 内の byte 順だけを反転する。
    raw = b"".join(word[::-1] for word in sfrbx.words_ubx[:8])
    received = read_bits(raw, CRC_DATA_BITS, 24)
    extra = sfrbx.words_ubx[8] if sfrbx.num_words == 9 else None
    return VerifiedL1s(sfrbx, raw, extra, raw[0], read_bits(raw, 8, 6),
                       received, crc24(raw), read_bits(raw, 250, 6, 256))


@dataclass(frozen=True)
class VerificationComparison:
    verified: VerifiedL1s | None
    mismatch: bool
    reasons: tuple[str, ...]


def compare_claim(claim: ClaimedL1sFrame) -> VerificationComparison:
    try:
        verified = verify_ubx_l1s(claim.ubx)
    except L1sError as exc:
        return VerificationComparison(None, True, (str(exc),))
    reasons: list[str] = []
    actual_flags = FLAG_UBX_VALID | FLAG_LENGTH_VALID
    if verified.valid_pab:
        actual_flags |= FLAG_PAB_VALID
    if verified.valid_crc:
        actual_flags |= FLAG_CRC_VALID
    if (claim.claimed_flags & 0x0F) != actual_flags:
        reasons.append("STM32 validity flags disagree with Python")
    for name, claimed, actual in (
        ("svId", claim.sv_id, verified.sfrbx.sv_id),
        ("gnssId", claim.gnss_id, verified.sfrbx.gnss_id),
        ("sigId", claim.sig_id, verified.sfrbx.sig_id),
        ("numWords", claim.num_words, verified.sfrbx.num_words),
        ("PAB", claim.pab, verified.pab),
        ("MT", claim.message_type, verified.message_type),
    ):
        if claimed != actual:
            reasons.append(f"claimed {name}={claimed} calculated={actual}")
    return VerificationComparison(verified, bool(reasons), tuple(reasons))
