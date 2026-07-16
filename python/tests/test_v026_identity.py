from dataclasses import replace
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from hoshiguard.bridge_frames import L1sFrame, parse_payload
from hoshiguard.bridge_protocol import BridgeFrame, FrameType
from hoshiguard.events.canonical import (
    SEMANTIC_FINGERPRINT_VERSION, canonical_json, canonical_semantic,
    semantic_fingerprint,
)
from hoshiguard.events.state_machine import EventProcessor
from hoshiguard.l1s import verify_ubx_l1s
from hoshiguard.models import AlertArea, AlertIdentity, UnknownField
from hoshiguard.output.ndjson import NdjsonWriter
from hoshiguard.output.raw_archive import RawArchiveReader
from hoshiguard.routing import make_alert

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = json.loads((ROOT / "test/fixtures/real/manifest.json").read_text())


def fixture(name):
    return next(item for item in MANIFEST["fixtures"] if item["name"] == name)


def alert_for(name, sequence=1, timestamp=100):
    verified = verify_ubx_l1s(bytes.fromhex(fixture(name)["ubx_hex"]))
    claim = L1sFrame(1, verified.sfrbx.sv_id, verified.sfrbx.gnss_id,
                    verified.sfrbx.sig_id, verified.sfrbx.num_words,
                    verified.pab, verified.message_type, 0x0F,
                    verified.sfrbx.packet.raw, 0, 0, "0.2.6")
    bridge = BridgeFrame(1, 0, FrameType.L1S_FRAME, 0, sequence, timestamp,
                         b"", 0)
    return make_alert(bridge, claim, verified)


def real_marine_alert():
    path = ROOT / "test/fixtures/bridge_real_session/selected_records.hgraw"
    for record in RawArchiveReader(str(path)):
        if record.frame.frame_type != FrameType.L1S_FRAME:
            continue
        claim = parse_payload(record.frame)
        if claim.message_type == 43:
            verified = verify_ubx_l1s(claim.ubx)
            alert = make_alert(record.frame, claim, verified)
            if alert.category.code == 14:
                return alert
    raise AssertionError("selected real session fixture lacks Marine MT43")


class SemanticIdentityV2Tests(unittest.TestCase):
    def test_version_is_explicit(self):
        alert = alert_for("qzss_l1s_mt43_pab53")
        self.assertEqual(alert.identity.semantic_fingerprint_version,
                         SEMANTIC_FINGERPRINT_VERSION)
        self.assertEqual(SEMANTIC_FINGERPRINT_VERSION, 2)

    def test_same_dcr_payload_different_pab_is_same_semantic(self):
        base = real_marine_alert()
        alerts = [replace(base, receipt=replace(base.receipt, pab=pab),
                          raw=replace(base.raw,
                                      l1s_hex=f"{pab:02X}" + base.raw.l1s_hex[2:]))
                  for pab in (0x53, 0x9A, 0xC6)]
        self.assertEqual(len({semantic_fingerprint(x) for x in alerts}), 1)
        self.assertEqual(len({x.raw.l1s_hex for x in alerts}), 3)

    def test_receipt_sv_sequence_timestamp_do_not_change_semantics(self):
        alert = alert_for("qzss_l1s_mt43_pab53")
        changed = replace(alert, receipt=replace(
            alert.receipt, sv_id=7, bridge_sequence=999,
            timestamp_us=987654321, pps_sequence=44))
        self.assertEqual(semantic_fingerprint(alert), semantic_fingerprint(changed))

    def test_raw_ubx_wrapper_does_not_change_semantics(self):
        alert = alert_for("qzss_l1s_mt43_pab53")
        changed = replace(alert, raw=replace(alert.raw,
                                             ubx_hex="00" + alert.raw.ubx_hex[2:],
                                             extra_word_hex="DEADBEEF"))
        self.assertEqual(semantic_fingerprint(alert), semantic_fingerprint(changed))

    def test_transport_labels_are_absent_from_dcr_canonical(self):
        value = canonical_json(canonical_semantic(alert_for("qzss_l1s_mt43_pab53")))
        for forbidden in ("nmea", "sentence", "preamble", "satellite_id",
                          "timestamp"):
            self.assertNotIn(f'"{forbidden}"', value)

    def test_region_change_changes_semantics(self):
        alert = alert_for("qzss_l1s_mt43_pab53")
        fields = dict(alert.payload.fields)
        fields["marine_forecast_regions_raw"] = [9999]
        changed = replace(alert, payload=replace(alert.payload,
                                                 fields=tuple(sorted(fields.items()))))
        self.assertNotEqual(semantic_fingerprint(alert), semantic_fingerprint(changed))

    def test_status_and_action_change_semantics(self):
        alert = alert_for("qzss_l1s_mt43_pab53")
        self.assertNotEqual(semantic_fingerprint(alert),
                            semantic_fingerprint(replace(alert, status="test")))
        self.assertNotEqual(semantic_fingerprint(alert),
                            semantic_fingerprint(replace(alert, action="clear")))

    def test_category_code_change_changes_semantics(self):
        alert = alert_for("qzss_l1s_mt43_pab53")
        changed = replace(alert, category=replace(alert.category, code=99,
                                                  token="FUTURE_99"))
        self.assertNotEqual(semantic_fingerprint(alert), semantic_fingerprint(changed))

    def test_dcx_provider_change_changes_semantics(self):
        alert = alert_for("qzss_l1s_mt44_pab53")
        camf = replace(alert.payload.camf,
                       a3_provider=alert.payload.camf.a3_provider + 1)
        changed = replace(alert, payload=replace(alert.payload, camf=camf))
        self.assertNotEqual(semantic_fingerprint(alert), semantic_fingerprint(changed))

    def test_meaningful_unknown_raw_changes_semantics(self):
        alert = alert_for("qzss_l1s_mt43_pab53")
        one = replace(alert, unknown_fields=(UnknownField("future", 1, 100, 2),))
        two = replace(alert, unknown_fields=(UnknownField("future", 2, 100, 2),))
        self.assertNotEqual(semantic_fingerprint(one), semantic_fingerprint(two))

    def test_area_set_is_canonical_sorted_and_names_are_presentation(self):
        alert = alert_for("qzss_l1s_mt43_pab53")
        a = AlertArea(2, "two", "region", raw=2)
        b = AlertArea(1, "one", "region", raw=1)
        first = replace(alert, areas=(a, b))
        second = replace(alert, areas=(replace(b, name="uno"),
                                       replace(a, name="dos")))
        self.assertEqual(semantic_fingerprint(first), semantic_fingerprint(second))

    def test_ordered_payload_lists_are_not_sorted(self):
        alert = real_marine_alert()
        fields = dict(alert.payload.fields)
        values = fields["marine_forecast_regions_raw"]
        fields["marine_forecast_regions_raw"] = list(reversed(values))
        changed = replace(alert, payload=replace(alert.payload,
                                                 fields=tuple(sorted(fields.items()))))
        self.assertNotEqual(semantic_fingerprint(alert), semantic_fingerprint(changed))

    def test_fingerprint_is_stable_in_fresh_process(self):
        expected = alert_for("qzss_l1s_mt43_pab53").identity.semantic_fingerprint
        script = ("import sys; sys.path.insert(0, 'python/tests'); "
                  "from test_v026_identity import alert_for; "
                  "print(alert_for('qzss_l1s_mt43_pab53').identity.semantic_fingerprint)")
        result = subprocess.run([sys.executable, "-c", script], cwd=ROOT,
                                text=True, capture_output=True, check=True)
        self.assertEqual(result.stdout.strip(), expected)

    def test_marine_repeats_do_not_repeat_issued(self):
        processor = EventProcessor()
        base = real_marine_alert()
        other_pab = 0x53 if base.receipt.pab != 0x53 else 0x9A
        first = processor.observe(base)
        altered = replace(base,
                          receipt=replace(base.receipt, pab=other_pab,
                                          timestamp_us=base.receipt.timestamp_us + 100),
                          identity=replace(base.identity,
                              frame_fingerprint="9" * 64))
        repeat = processor.observe(altered)
        self.assertEqual(first.disposition, "issued")
        self.assertEqual(repeat.disposition, "repeated")
        self.assertEqual(repeat.repeat_count, 2)
        self.assertEqual(repeat.pabs_seen,
                         tuple(sorted((base.receipt.pab, other_pab))))

    def test_same_event_key_different_semantic_becomes_update(self):
        alert = alert_for("qzss_l1s_mt43_pab53")
        processor = EventProcessor()
        processor.observe(alert)
        changed_identity = replace(alert.identity, semantic_fingerprint="f" * 64)
        changed = replace(alert, identity=changed_identity,
                          receipt=replace(alert.receipt, timestamp_us=200))
        self.assertEqual(processor.observe(changed).disposition, "updated")
        self.assertEqual(alert.identity.event_key, changed.identity.event_key)

    def test_real_same_semantic_pab_frames_keep_distinct_frame_fingerprint(self):
        first = alert_for("qzss_l1s_mt43_pab53")
        second = alert_for("qzss_l1s_mt43_pabc6")
        self.assertEqual(first.identity.semantic_fingerprint,
                         second.identity.semantic_fingerprint)
        self.assertNotEqual(first.identity.frame_fingerprint,
                            second.identity.frame_fingerprint)

    def test_null_and_slas_never_create_events(self):
        null = alert_for("qzss_l1s_mt44_pab53")
        slas = alert_for("qzss_l1s_mt47")
        processor = EventProcessor()
        for value in (null, slas):
            self.assertFalse(value.alert)
            self.assertEqual(processor.observe(value).disposition, "observed")
        self.assertEqual(len(processor.events), 0)

    def test_null_fingerprint_stable_across_pab(self):
        alerts = [alert_for("qzss_l1s_mt44_" + suffix)
                  for suffix in ("pab53", "pab9a", "pabc6")]
        self.assertEqual(len({x.identity.semantic_fingerprint for x in alerts}), 1)

    def test_repeat_aggregate_tracks_seen_evidence(self):
        base = alert_for("qzss_l1s_mt43_pab53")
        processor = EventProcessor()
        processor.observe(base)
        altered = replace(base,
                          receipt=replace(base.receipt, sv_id=7, pab=0xC6,
                                          timestamp_us=300),
                          identity=replace(base.identity,
                              frame_fingerprint="a" * 64))
        result = processor.observe(altered)
        self.assertEqual(result.satellites_seen, (3, 7))
        self.assertEqual(result.pabs_seen, (0x53, 0xC6))
        self.assertEqual(result.frame_fingerprints_seen, 2)
        self.assertEqual(result.first_seen_us, 100)
        self.assertEqual(result.last_seen_us, 300)

    def test_repeat_summary_is_bounded_and_versioned(self):
        alert = alert_for("qzss_l1s_mt43_pab53")
        processor = EventProcessor(capacity=1, maximum_frame_fingerprints=1)
        processor.observe(alert); processor.observe(alert)
        summary = processor.repeat_summaries()[0]
        self.assertEqual(summary.semantic_fingerprint_version, 2)
        self.assertEqual(summary.frame_fingerprints_seen, 1)

    def test_localized_category_names_do_not_change_semantics(self):
        alert = alert_for("qzss_l1s_mt43_pab53")
        changed = replace(alert, category=replace(
            alert.category, official_ja="表示変更", official_en="Display only",
            chinese="仅显示"))
        self.assertEqual(semantic_fingerprint(alert), semantic_fingerprint(changed))

    def test_ttl_and_capacity_are_bounded(self):
        alert = alert_for("qzss_l1s_mt43_pab53")
        processor = EventProcessor(capacity=1, ttl_us=10)
        processor.observe(alert)
        later = replace(alert, receipt=replace(alert.receipt, timestamp_us=1000))
        self.assertEqual(processor.observe(later).repeat_count, 1)
        self.assertGreaterEqual(processor.evictions, 1)

    def test_test_and_actual_event_spaces_are_isolated(self):
        actual = alert_for("qzss_l1s_mt43_pab53")
        test_identity = replace(actual.identity,
                                event_key=actual.identity.event_key.replace(
                                    ":actual:", ":test:"),
                                semantic_fingerprint="b" * 64)
        test = replace(actual, status="test", identity=test_identity,
                       receipt=replace(actual.receipt, timestamp_us=200))
        processor = EventProcessor()
        self.assertEqual(processor.observe(actual).disposition, "issued")
        self.assertEqual(processor.observe(test).disposition, "issued")

    def test_ndjson_records_stream_type_and_same_model(self):
        result = EventProcessor().observe(alert_for("qzss_l1s_mt43_pab53"))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "one.ndjson"
            with NdjsonWriter(str(path)) as writer:
                writer.write(result)
            value = json.loads(path.read_text())
        self.assertEqual(value["record_type"], result.record_type)
        self.assertEqual(value["identity"]["semantic_fingerprint_version"], 2)

    def test_events_only_cli_excludes_null_and_slas_lifecycle(self):
        fixture_path = ROOT / "test/fixtures/bridge_real_session/selected_records.hgraw"
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "events.ndjson"
            subprocess.run([sys.executable, "-m", "hoshiguard", "--bridge-file",
                            str(fixture_path), "--events-only", "--ndjson",
                            str(output)], cwd=ROOT, check=True, capture_output=True)
            values = [json.loads(line) for line in output.read_text().splitlines()
                      if json.loads(line).get("record_type") == "event_lifecycle"]
            self.assertTrue(values)
            self.assertTrue(all(item["alert"] for item in values))
            self.assertFalse({"SLAS", "NULL"} & {item["service"] for item in values})


if __name__ == "__main__":
    unittest.main()
