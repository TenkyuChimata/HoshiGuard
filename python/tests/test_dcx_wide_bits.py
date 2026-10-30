from dataclasses import replace
import json
from pathlib import Path
import unittest

from hoshiguard.dcx.camf import decode_camf
from hoshiguard.dcx.decoder import decode_dcx
from hoshiguard.dcx.extended import decode_extended
from hoshiguard.l1s import L1sError, crc24, read_bits, verify_ubx_l1s


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = json.loads((ROOT / "test/fixtures/real/manifest.json").read_text())
MT44 = next(item for item in MANIFEST["fixtures"]
            if item["name"] == "qzss_l1s_mt44_pab53")
REAL_EX9 = json.loads((ROOT / "test/fixtures/real/dcx_j_alert_ex9_20260804.json")
                       .read_text())
EX9_VALUE = 0x0123456789ABCDEF


def set_bits(data: bytearray, offset: int, width: int, value: int) -> None:
    if value < 0 or value >= (1 << width):
        raise ValueError("value does not fit")
    for index in range(width):
        bit = (value >> (width - index - 1)) & 1
        position = offset + index
        mask = 1 << (7 - (position & 7))
        if bit:
            data[position >> 3] |= mask
        else:
            data[position >> 3] &= ~mask


def reference_bits(data: bytes, offset: int, width: int) -> int:
    bits = "".join(f"{byte:08b}" for byte in data)
    return int(bits[offset:offset + width] or "0", 2)


def j_alert_raw(provider: int) -> bytes:
    raw = bytearray(32)
    set_bits(raw, 0, 8, 0x53)
    set_bits(raw, 8, 6, 44)
    set_bits(raw, 24, 2, 1)       # A1
    set_bits(raw, 26, 9, 111)     # A2 日本
    set_bits(raw, 35, 5, provider)
    set_bits(raw, 40, 7, 44)      # A4
    set_bits(raw, 47, 2, 2)       # A5
    set_bits(raw, 146, 1, 1)      # EX8
    set_bits(raw, 147, 64, EX9_VALUE)
    set_bits(raw, 211, 3, 5)      # EX10
    set_bits(raw, 214, 6, 7)      # ライブラリ版
    set_bits(raw, 226, 24, crc24(bytes(raw)))
    return bytes(raw)


class WideBitReaderTests(unittest.TestCase):
    def test_aligned_64_bit_read(self):
        data = bytes.fromhex("0123456789ABCDEF")
        self.assertEqual(read_bits(data, 0, 64, 64), EX9_VALUE)

    def test_unaligned_64_bit_read(self):
        data = bytes.fromhex("F123456789ABCDEFAA")
        self.assertEqual(read_bits(data, 3, 64, 72),
                         reference_bits(data, 3, 64))

    def test_real_ex9_offset_147_width_64(self):
        raw = j_alert_raw(2)
        self.assertEqual(read_bits(raw, 147, 64), EX9_VALUE)

    def test_ex9_matches_two_32_bit_reads(self):
        raw = j_alert_raw(3)
        split = (read_bits(raw, 147, 32) << 32) | read_bits(raw, 179, 32)
        self.assertEqual(read_bits(raw, 147, 64), split)

    def test_default_250_bit_boundary_is_still_enforced(self):
        raw = bytes(32)
        with self.assertRaises(L1sError):
            read_bits(raw, 200, 51)
        self.assertEqual(read_bits(raw, 250, 0), 0)
        with self.assertRaises(L1sError):
            read_bits(raw, 251, 0)

    def test_bit_length_cannot_exceed_storage(self):
        with self.assertRaises(L1sError):
            read_bits(b"\0" * 8, 0, 1, 65)

    def test_negative_arguments_are_rejected(self):
        data = bytes(32)
        for arguments in ((-1, 1, 250), (0, -1, 250), (0, 0, -1)):
            with self.subTest(arguments=arguments), self.assertRaises(L1sError):
                read_bits(data, *arguments)

    def test_non_integer_bounds_are_rejected(self):
        with self.assertRaises(L1sError):
            read_bits(bytes(32), 0, 1.5, 250)

    def test_existing_common_widths_are_unchanged(self):
        data = bytes(range(32))
        for width, offset in ((8, 5), (16, 13), (17, 31),
                              (24, 79), (32, 121)):
            with self.subTest(width=width):
                self.assertEqual(read_bits(data, offset, width),
                                 reference_bits(data, offset, width))

    def test_provider_2_and_3_ex8_ex9_ex10_decode(self):
        real = verify_ubx_l1s(bytes.fromhex(MT44["ubx_hex"]))
        for provider in (2, 3):
            with self.subTest(provider=provider):
                raw = j_alert_raw(provider)
                camf = decode_camf(raw)
                extended = decode_extended(raw, camf)
                fields = dict(extended.raw_fields)
                self.assertEqual(extended.variant, "j_alert_ex8_ex9")
                self.assertEqual(fields["EX8_target_area_list_type"], 1)
                self.assertEqual(fields["EX9_target_area_code_list"], EX9_VALUE)
                self.assertEqual(fields["EX10_reserved"], 5)
                payload, *_rest = decode_dcx(replace(real, raw=raw))
                self.assertEqual(dict(payload.extended.raw_fields), fields)

    def test_real_provider_3_j_alert_ex9_fixture(self):
        frame = verify_ubx_l1s(bytes.fromhex(REAL_EX9["ubx_hex"]))
        payload, category, status, action, _fields, _areas = decode_dcx(frame)
        fields = dict(payload.extended.raw_fields)
        self.assertTrue(frame.valid_crc)
        self.assertEqual(frame.message_type, 44)
        self.assertEqual(payload.camf.a3_provider, 3)
        self.assertEqual(payload.extended.variant, "j_alert_ex8_ex9")
        self.assertEqual(fields["EX8_target_area_list_type"], 0)
        self.assertEqual(fields["EX9_target_area_code_list"],
                         REAL_EX9["ex9_target_area_code_list"])
        self.assertEqual(fields["EX10_reserved"], 0)
        self.assertEqual(category.code, 95)
        self.assertEqual((status, action), ("test", "issue"))


if __name__ == "__main__":
    unittest.main()
