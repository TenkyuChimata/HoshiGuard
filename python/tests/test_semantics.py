import json
from pathlib import Path
import unittest

from hoshiguard.bridge_frames import L1sFrame
from hoshiguard.bridge_protocol import BridgeFrame, FrameType
from hoshiguard.dcr.decoder import decode_dcr
from hoshiguard.dcr.models import PAYLOAD_TYPES
from hoshiguard.dcr.reassembly import NankaiTroughReassembler
from hoshiguard.dcr.tables import supported_categories
from hoshiguard.dcx.camf import CAMF_TRACE, decode_camf
from hoshiguard.dcx.areas import areas_from_extended
from hoshiguard.dcx.decoder import decode_dcx
from hoshiguard.dcx.extended import decode_extended
from hoshiguard.dcx.reassembly import requires_reassembly
from hoshiguard.dcx.tables import all_hazards
from hoshiguard.events import EventProcessor
from hoshiguard.l1s import verify_ubx_l1s
from hoshiguard.models import AlertService, json_safe
from hoshiguard.output.human import format_human
from hoshiguard.routing import make_alert, service_for_mt

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = json.loads((ROOT / "test/fixtures/real/manifest.json").read_text())


def fixture(name):
    return next(item for item in MANIFEST["fixtures"] if item["name"] == name)


def claim_for(item):
    verified = verify_ubx_l1s(bytes.fromhex(item["ubx_hex"]))
    return verified, L1sFrame(0, verified.sfrbx.sv_id, 5, 1,
                              verified.sfrbx.num_words, verified.pab,
                              verified.message_type, 0x0F,
                              verified.sfrbx.packet.raw, 0, 0, "0.2.5")


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

    def test_non_alert_services_never_enter_issued_lifecycle(self):
        frame, claim = claim_for(fixture("qzss_l1s_mt48"))
        bridge = BridgeFrame(1, 0, FrameType.L1S_FRAME, 0, 8, 9, b"", 0)
        alert = make_alert(bridge, claim, frame)
        self.assertFalse(alert.alert)
        self.assertEqual(alert.action, "none")
        result = EventProcessor().observe(alert)
        self.assertEqual(result.disposition, "observed")
        self.assertNotIn("Action: issued", format_human(result))

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

    def test_frame_semantic_and_event_dedup_are_distinct(self):
        frame, claim = claim_for(fixture("qzss_l1s_mt43_pab53"))
        bridge = BridgeFrame(1, 0, FrameType.L1S_FRAME, 0, 8, 9, b"", 0)
        alert = make_alert(bridge, claim, frame)
        processor = EventProcessor()
        self.assertEqual(processor.observe(alert).disposition, "issued")
        repeated = processor.observe(alert)
        self.assertEqual(repeated.disposition, "repeated")
        self.assertEqual(repeated.repeat_count, 2)


if __name__ == "__main__":
    unittest.main()
