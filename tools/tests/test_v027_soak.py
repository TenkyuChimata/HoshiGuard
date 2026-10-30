import json
from pathlib import Path
import tempfile
import unittest

from hoshiguard.bridge_frames import FrameType
from hoshiguard.output.raw_archive import RawArchiveReader
from hoshiguard.soak_analysis import analyze_soak

from tools.audit_ndjson_stream import audit

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "test/fixtures/bridge_24h_session"
SELECTED = FIXTURE / "selected_records.hgraw"
MANIFEST = json.loads((FIXTURE / "manifest.json").read_text())
EXPECTED = json.loads((FIXTURE / "expected_summary.json").read_text())


class V027SoakTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.report = analyze_soak(str(SELECTED), hourly_seconds=3600)

    def test_fixture_source_is_real(self):
        self.assertEqual(MANIFEST["evidence_source"], "real_hardware")

    def test_fixture_records_source_firmware(self):
        self.assertEqual(MANIFEST["source_session"]["firmware"], "0.2.5")

    def test_fixture_protocol_is_one_zero(self):
        self.assertEqual(MANIFEST["source_session"]["bridge_protocol"], "1.0")

    def test_fixture_does_not_claim_current_firmware_soak(self):
        text = (ROOT / "test/fixtures/README.md").read_text(encoding="utf-8")
        self.assertIn("現在のファームウェアの 24 時間実機試験完了を意味しません", text)

    def test_fixture_contains_hourly_representatives(self):
        reasons = [reason for item in MANIFEST["selected_records"]
                   for reason in item["reasons"]]
        hours = [reason for reason in reasons if reason.startswith("hour ")]
        self.assertGreaterEqual(len(hours), 24)

    def test_fixture_contains_first_and_last_hello(self):
        reasons = {reason for item in MANIFEST["selected_records"]
                   for reason in item["reasons"]}
        self.assertIn("first HELLO", reasons)
        self.assertIn("last HELLO", reasons)

    def test_fixture_contains_first_and_last_status(self):
        reasons = {reason for item in MANIFEST["selected_records"]
                   for reason in item["reasons"]}
        self.assertIn("first STATUS", reasons)
        self.assertIn("last STATUS", reasons)

    def test_fixture_contains_first_and_last_time_status(self):
        reasons = {reason for item in MANIFEST["selected_records"]
                   for reason in item["reasons"]}
        self.assertIn("first TIME_STATUS", reasons)
        self.assertIn("last TIME_STATUS", reasons)

    def test_fixture_contains_every_observed_mt(self):
        actual = {item.get("mt") for item in MANIFEST["selected_records"]
                  if "mt" in item}
        self.assertTrue({43, 44, 47, 48, 49, 50} <= actual)

    def test_fixture_contains_every_observed_satellite(self):
        actual = {item.get("sv_id") for item in MANIFEST["selected_records"]
                  if "sv_id" in item}
        self.assertTrue({2, 3, 7} <= actual)

    def test_fixture_contains_all_pabs(self):
        actual = {item.get("pab") for item in MANIFEST["selected_records"]
                  if "pab" in item}
        self.assertTrue({"53", "9A", "C6"} <= actual)

    def test_fixture_contains_maximum_gap_boundaries(self):
        reasons = {reason for item in MANIFEST["selected_records"]
                   for reason in item["reasons"]}
        self.assertIn("before maximum L1S gap", reasons)
        self.assertIn("after maximum L1S gap", reasons)

    def test_fixture_contains_queue_watermark_changes(self):
        reasons = [reason for item in MANIFEST["selected_records"]
                   for reason in item["reasons"]]
        self.assertTrue(any("high-watermark became 4" in reason for reason in reasons))

    def test_selected_archive_is_stream_replayable(self):
        records = list(RawArchiveReader(str(SELECTED)))
        self.assertEqual(len(records), len(MANIFEST["selected_records"]))

    def test_selected_wire_hashes_match_manifest(self):
        from hashlib import sha256
        records = list(RawArchiveReader(str(SELECTED)))
        hashes = [sha256(record.wire).hexdigest() for record in records]
        self.assertEqual(hashes, [item["wire_sha256"]
                                  for item in MANIFEST["selected_records"]])

    def test_analyzer_streams_selected_records(self):
        self.assertEqual(self.report["records"],
                         len(MANIFEST["selected_records"]))

    def test_analyzer_hour_buckets_exist(self):
        self.assertGreaterEqual(len(self.report["hourly"]), 24)

    def test_analyzer_reports_periodic_hello_without_epoch_reset(self):
        self.assertTrue(self.report["hello"]["periodic_not_epoch"])
        self.assertEqual(self.report["sequence"]["epochs"], 1)

    def test_analyzer_checks_pc_timestamp_monotonicity(self):
        self.assertEqual(self.report["timestamps"]["pc_nonmonotonic"], 0)

    def test_analyzer_reports_maximum_intervals(self):
        self.assertGreater(self.report["timestamps"]["maximum_l1s_gap"]["seconds"], 0)

    def test_analyzer_reports_frame_byte_ratios(self):
        ratios = self.report["throughput"]["frame_type_byte_ratio"]
        self.assertAlmostEqual(sum(ratios.values()), 1.0)

    def test_analyzer_reports_semantic_collision_metrics(self):
        self.assertEqual(self.report["semantic"]["hash_collision_count"], 0)
        self.assertEqual(self.report["semantic"]["canonical_mismatch_count"], 0)

    def test_analyzer_keeps_null_out_of_events(self):
        self.assertEqual(self.report["events"]["null_in_event_stream"], 0)

    def test_analyzer_keeps_slas_out_of_events(self):
        self.assertEqual(self.report["events"]["slas_in_event_stream"], 0)

    def test_analyzer_extracts_dcr_rc_and_it_from_canonical_payload(self):
        coverage = self.report["coverage"]
        self.assertTrue(coverage["dcr_rc"])
        self.assertTrue(coverage["dcr_it"])

    def test_expected_summary_has_zero_drop_evidence(self):
        self.assertTrue(EXPECTED["zero_l1s_drop_proven"])

    def test_expected_summary_sequence_is_contiguous(self):
        sequence = EXPECTED["sequence"]
        self.assertEqual((sequence["gaps"], sequence["duplicates"],
                          sequence["out_of_order"]), (0, 0, 0))

    def test_audit_infers_semantic_mode_from_record_type(self):
        records = [record for record in RawArchiveReader(str(SELECTED))
                   if record.frame.frame_type == FrameType.L1S_FRAME]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "misnamed-all.ndjson"
            path.write_text(json.dumps({
                "record_type": "semantic_observation", "service": "DCR",
                "receipt": {"bridge_sequence": records[0].frame.sequence, "mt": 43},
                "identity": {"semantic_fingerprint": "a", "frame_fingerprint": "b",
                             "event_key": "c"}}) + "\n")
            result = audit(str(path), str(SELECTED))
        self.assertEqual(result["actual_stream_mode"], "semantic_observations")

    def test_audit_prefers_explicit_stream_mode(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "out.ndjson"
            path.write_text(json.dumps({"record_type": "session_start",
                                        "stream_mode": "events_only"}) + "\n")
            result = audit(str(path), str(SELECTED))
        self.assertEqual(result["mode_evidence"], "explicit_metadata")

    def test_audit_detects_invalid_json(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "out.ndjson"
            path.write_text("{bad}\n")
            result = audit(str(path), str(SELECTED))
        self.assertEqual(result["invalid_json_lines"], 1)

    def test_audit_detects_duplicate_output_lines(self):
        line = json.dumps({"record_type": "semantic_observation"}) + "\n"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "out.ndjson"
            path.write_text(line + line)
            result = audit(str(path), str(SELECTED))
        self.assertEqual(result["duplicate_output_lines"], 1)

    def test_audit_reports_sequences_outside_archive(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "out.ndjson"
            path.write_text(json.dumps({"record_type": "frame_observation",
                                        "receipt": {"bridge_sequence": 0xFFFFFFFF}}) + "\n")
            result = audit(str(path), str(SELECTED))
        self.assertEqual(result["output_sequences_outside_archive"], [0xFFFFFFFF])


if __name__ == "__main__":
    unittest.main()
