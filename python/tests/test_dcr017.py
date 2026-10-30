"""Explicitly synthetic, CRC-correct DCR-017 vectors from normative layouts."""
from dataclasses import replace
import json
from pathlib import Path
import unittest

from hoshiguard.bridge_frames import parse_payload
from hoshiguard.bridge_protocol import decode_wire, encode_frame, FrameType
from hoshiguard.dcr.decoder import DcrDecodeError, decode_dcr
from hoshiguard.events.canonical import semantic_fingerprint
from hoshiguard.l1s import crc24, read_bits, verify_ubx_l1s
from hoshiguard.models import json_safe
from hoshiguard.public import PublicAlert
from hoshiguard.publisher import AlertStore
from hoshiguard.runtime import ReceiverPipeline
from hoshiguard.ubx import checksum


def bits(raw, offset, width, value):
    for i in range(width):
        mask = 1 << (7 - (offset + i) % 8)
        raw[(offset + i) // 8] = (raw[(offset + i) // 8] & ~mask) | (((value >> (width - i - 1)) & 1) * mask)


def synthetic_dcr(category=5, *, arrival=(0, 30, 62), height=13,
                  classification=3, information=0, warning=3, region=100):
    raw = bytearray(32)
    for offset, width, value in [(0, 8, 0x53), (8, 6, 43), (14, 3, classification),
                                 (17, 4, category), (21, 4, 7), (25, 5, 15),
                                 (30, 5, 2), (35, 6, 35), (41, 2, information), (214, 6, 1)]:
        bits(raw, offset, width, value)
    if category == 5:
        for offset, width, value in [(80, 4, warning), (84, 1, arrival[0]), (85, 5, arrival[1]),
                                     (90, 6, arrival[2]), (96, 4, height), (100, 10, region)]:
            bits(raw, offset, width, value)
    elif category == 11:
        bits(raw, 53, 4, warning)
        bits(raw, 57, 40, region)
    elif category == 10:
        bits(raw, 53, 3, warning)
        bits(raw, 56, 5, 1)
        bits(raw, 61, 19, 11000)
    return wire_for_raw(raw)


def wire_for_raw(raw):
    bits(raw, 226, 24, crc24(raw))
    words = b''.join(raw[i:i+4][::-1] for i in range(0, 32, 4))
    payload = bytes((5, 3, 1, 0, 8, 0, 2, 0)) + words
    ubx = b'\xb5\x62\x02\x13' + len(payload).to_bytes(2, 'little') + payload
    ubx += bytes(checksum(ubx[2:]))
    metadata = bytearray(20)
    metadata[4:10] = bytes((3, 5, 1, 8, raw[0], read_bits(raw, 8, 6)))
    metadata[10:12] = (15).to_bytes(2, 'little')
    metadata[12:14] = len(ubx).to_bytes(2, 'little')
    return decode_wire(encode_frame(FrameType.L1S_FRAME, 1, 1, bytes(metadata) + ubx))


def decoded(frame):
    return decode_dcr(verify_ubx_l1s(parse_payload(frame).ubx))


class Dcr017Tests(unittest.TestCase):
    def test_new_absent_time_height13_and_old_arrived_remain_distinct(self):
        absent = decoded(synthetic_dcr())[-1]
        arrived = decoded(synthetic_dcr(arrival=(0, 31, 63), height=14))[-1]
        self.assertIsNone(absent['expected_tsunami_arrival_times'][0])
        self.assertIsNone(arrived['expected_tsunami_arrival_times'][0])
        self.assertEqual(absent['tsunami_heights'][0], '該当情報なし')
        self.assertEqual(arrived['tsunami_heights'][0], '不明')
        self.assertEqual(absent['expected_tsunami_arrival_times_raw'][0]['hour'], 30)
        self.assertEqual(arrived['expected_tsunami_arrival_times_raw'][0]['hour'], 31)

    def test_invalid_mixed_sentinels_rejected(self):
        for arrival in ((0, 30, 63), (0, 31, 62), (1, 30, 62), (0, 10, 63)):
            with self.subTest(arrival=arrival), self.assertRaises(DcrDecodeError):
                decoded(synthetic_dcr(arrival=arrival))

    def test_old_numeric_time_height_still_decodes(self):
        fields = decoded(synthetic_dcr(arrival=(0, 12, 30), height=3))[-1]
        self.assertIsNotNone(fields['expected_tsunami_arrival_times'][0])
        self.assertEqual(fields['tsunami_heights_raw'], [3])

    def test_five_flood_additions_and_renames(self):
        expected = {880801000200:'石手川', 880802000103:'菅田', 890907000103:'中流部',
                    890907000100:'下流部', 890917000100:'水系'}
        for code, name in expected.items():
            fields = decoded(synthetic_dcr(11, warning=4, region=code))[-1]
            self.assertIn(name, fields['flood_forecast_regions'][0])
            self.assertEqual(fields['flood_forecast_regions_raw'], [code])

    def test_cancel_dominates_service_clear(self):
        for category, warning in ((5, 2), (11, 1), (10, 2)):
            result = decoded(synthetic_dcr(category, warning=warning, information=2))
            self.assertEqual(result[3], 'cancel')
            result = decoded(synthetic_dcr(category, warning=warning))
            self.assertEqual(result[3], 'clear')

    def test_unknown_classification_never_actual_or_published(self):
        frame = synthetic_dcr(classification=0)
        self.assertEqual(decoded(frame)[2], 'unknown')
        store = AlertStore()
        self.assertIsNone(ReceiverPipeline(store).process(frame, 1784097827645520000))
        self.assertEqual(store.status()['alerts_cached'], 0)

    def test_new_sentinel_reaches_public_api_without_invented_values(self):
        store = AlertStore()
        record = ReceiverPipeline(store).process(synthetic_dcr(), 1784097827645520000)
        value = json.loads(record.encoded)
        self.assertEqual(value['provenance']['spec'], 'IS-QZSS-DCR-017')
        self.assertIsNone(value['payload']['fields']['expected_tsunami_arrival_times'][0])
        self.assertEqual(value['payload']['fields']['tsunami_heights_raw'], [13])

    def test_spec_label_does_not_rekey_unchanged_v2_semantics(self):
        from hoshiguard.routing import make_alert
        frame = synthetic_dcr()
        alert = make_alert(frame, parse_payload(frame), verify_ubx_l1s(parse_payload(frame).ubx))
        self.assertEqual(semantic_fingerprint(alert), semantic_fingerprint(replace(alert, spec='IS-QZSS-DCR-016')))


if __name__ == '__main__':
    unittest.main()
