"""DCR-017/DCX-004 の実受信、仕様ベクトル、境界値と再構成。合成値は実受信証拠と区別する。"""
from support import ROOT, MANIFEST, fixture, claim_for, set_bits, synthetic_dcr
from dataclasses import replace
import json
import unittest
from hoshiguard.bridge_frames import parse_payload
from hoshiguard.bridge_protocol import FrameType
from hoshiguard.dcr.decoder import DcrDecodeError, decode_dcr
from hoshiguard.events.canonical import semantic_fingerprint
from hoshiguard.l1s import crc24, read_bits, verify_ubx_l1s
from hoshiguard.models import json_safe
from hoshiguard.publisher import AlertStore
from hoshiguard.runtime import ReceiverPipeline
from hoshiguard.dcx.camf import decode_camf
from hoshiguard.dcx.decoder import decode_dcx
from hoshiguard.dcx.extended import decode_extended
from hoshiguard.l1s import L1sError
from hoshiguard.bridge_protocol import BridgeFrame
from hoshiguard.dcr.models import PAYLOAD_TYPES
from hoshiguard.dcr.reassembly import NankaiTroughReassembler
from hoshiguard.dcr.tables import supported_categories
from hoshiguard.dcx.camf import CAMF_TRACE
from hoshiguard.dcx.areas import areas_from_extended
from hoshiguard.dcx.reassembly import requires_reassembly
from hoshiguard.dcx.tables import all_hazards
from hoshiguard.events import EventProcessor
from hoshiguard.models import AlertService
from hoshiguard.output.human import format_human
from hoshiguard.routing import make_alert, service_for_mt


def decoded(frame):
    return decode_dcr(verify_ubx_l1s(parse_payload(frame).ubx))


class DcrDecoderTests(unittest.TestCase):
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


MT44 = next(item for item in MANIFEST["fixtures"]
            if item["name"] == "qzss_l1s_mt44_pab53")
REAL_EX9 = json.loads((ROOT / "test/fixtures/real/dcx_j_alert_ex9_20260804.json")
                       .read_text())
EX9_VALUE = 0x0123456789ABCDEF


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


class SemanticTests(unittest.TestCase):
    def test_real_dcr_all_preamble_paths(self):
        for suffix in ("pab53", "pab9a", "pabc6"):
            frame, _ = claim_for(fixture("qzss_l1s_mt43_" + suffix))
            payload, category, status, action, _fields = decode_dcr(frame)
            self.assertIn(category.code, supported_categories())
            self.assertIsInstance(payload, PAYLOAD_TYPES[category.code])
            self.assertIn(status, ("actual", "test"))
            self.assertIn(action, ("issue", "update", "cancel"))

    def test_all_current_dcr_category_payloads_are_distinct(self):
        self.assertEqual(set(PAYLOAD_TYPES), set(supported_categories()))
        self.assertEqual(len(set(PAYLOAD_TYPES.values())), 12)

    def test_real_dcx_all_preamble_paths(self):
        for suffix in ("pab53", "pab9a", "pabc6"):
            frame, claim = claim_for(fixture("qzss_l1s_mt44_" + suffix))
            payload, category, status, action, _fields, _areas = decode_dcx(frame)
            self.assertEqual(payload.camf.a4_hazard, category.code)
            self.assertIn(status, ("actual", "test", "not_applicable"))
            self.assertIn(action, ("issue", "update", "clear", "none"))
            if status == "not_applicable":
                self.assertFalse(make_alert(
                    BridgeFrame(1, 0, FrameType.L1S_FRAME, 0, 1, 1, b"", 0),
                    claim, frame).alert)

    def test_camf_a1_through_a18_trace_is_complete(self):
        self.assertEqual(set(CAMF_TRACE), {f"A{i}" for i in range(1, 19)})
        raw = bytes.fromhex(fixture("qzss_l1s_mt44_pab53")["ubx_hex"])
        frame = verify_ubx_l1s(raw)
        fields = decode_camf(frame.raw)
        self.assertEqual(len(fields.__dataclass_fields__), 18)

    def test_all_113_camf_hazards_are_present(self):
        hazards = all_hazards()
        self.assertEqual(len(hazards), 113)
        self.assertEqual({item["code"] for item in hazards}, set(range(1, 114)))
        self.assertTrue(all(item["known"] for item in hazards))

    def test_extended_japan_and_outside_variants(self):
        frame, _ = claim_for(fixture("qzss_l1s_mt44_pab53"))
        camf = decode_camf(frame.raw)
        extended = decode_extended(frame.raw, camf)
        self.assertIn(extended.variant, ("null", "l_alert_ex1",
                                         "l_alert_main_ellipse", "j_alert_ex8_ex9",
                                         "local_government_ex1_ex7",
                                         "outside_japan_ex11"))
        outside = type(camf)(*([camf.a1_message_type, 1] +
                               list(camf.__dict__.values())[2:]))
        blocked = decode_extended(frame.raw, outside)
        self.assertEqual(blocked.variant, "outside_japan_ex11")
        self.assertTrue(blocked.blocked_by_missing_spec)

    def test_dcx_provider_routes_and_ex9_list_type_are_not_conflated(self):
        frame, _ = claim_for(fixture("qzss_l1s_mt44_pab53"))
        base = decode_camf(frame.raw)
        values = list(base.__dict__.values())

        values[1:5] = [111, 1, 1, 0]
        values[11:16] = [0, 0, 0, 0, 0]
        l_alert = decode_extended(frame.raw, type(base)(*values))
        self.assertEqual([name for name, _ in l_alert.raw_fields],
                         ["EX1_target_area_code"])

        values[2] = 4
        local = decode_extended(frame.raw, type(base)(*values))
        self.assertEqual(len(local.raw_fields), 7)

        from hoshiguard.dcx.models import ExtendedFields
        municipalities = areas_from_extended(ExtendedFields(
            "j_alert_ex8_ex9",
            (("EX8_target_area_list_type", 1),
             ("EX9_target_area_code_list", (13101 << 48) | (27100 << 32))),
            0))
        self.assertEqual([area.code for area in municipalities], [13101, 27100])

    def test_dcx_does_not_invent_cross_frame_reassembly(self):
        self.assertFalse(requires_reassembly())

    def test_nankai_reassembly_is_bounded_and_ordered(self):
        reassembly = NankaiTroughReassembler(capacity=1, timeout_seconds=10)
        self.assertIsNone(reassembly.add((1,), 2, 2, b"B", now=0))
        self.assertEqual(reassembly.add((1,), 1, 2, b"A", now=1), b"AB")
        self.assertIsNone(reassembly.add((2,), 1, 2, b"X", now=2))
        reassembly.expire(20)
        self.assertEqual(len(reassembly._items), 0)

    def test_mt_routing_does_not_create_empty_alert_for_slas_null(self):
        self.assertEqual(service_for_mt(47), (AlertService.SLAS, False,
                                               "IS-QZSS-L1S-009"))
        self.assertEqual(service_for_mt(63)[0], AlertService.NULL)
        self.assertFalse(service_for_mt(50)[1])


    def test_unified_human_json_share_same_model(self):
        frame, claim = claim_for(fixture("qzss_l1s_mt43_pab53"))
        bridge = BridgeFrame(1, 0, FrameType.L1S_FRAME, 0, 8, 9, b"", 0)
        alert = make_alert(bridge, claim, frame)
        result = EventProcessor().observe(alert)
        text = format_human(result)
        value = json_safe(alert)
        self.assertIn(alert.category.official_en or alert.category.token, text)
        self.assertEqual(value["schema"], "hoshiguard.alert.v1")
        json.dumps(value, allow_nan=False)

if __name__ == "__main__":
    unittest.main()
