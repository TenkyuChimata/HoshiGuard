"""実受信抜粋の完全性と、合成カウンターによる STATUS 境界の検証。"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(ROOT / "python"))
sys.path.insert(0, str(TOOLS))

import json
import tempfile
import unittest
import struct
from hoshiguard.bridge_frames import FrameType, parse_payload
from hoshiguard.bridge_protocol import (
    CobsError,
    CrcError,
    SequenceDisposition,
    SequenceTracker,
    decode_wire,
    encode_frame,
)
from hoshiguard.l1s import compare_claim
from hoshiguard.output.raw_archive import RawArchiveReader, RawArchiveWriter
from hoshiguard.session_validation import validate_session
from hoshiguard.soak_analysis import analyze_soak
from tools.audit_ndjson_stream import audit


SESSION = ROOT / "test/fixtures/bridge_real_session"


class RealBridgeSessionTests(unittest.TestCase):
    def test_synthetic_status_closure_and_pending_boundary(self):
        """合成カウンターで区間差分・未送信分・真の不一致を区別する。実機試験ではない。"""
        records = list(RawArchiveReader(str(SESSION / "selected_records.hgraw")))
        hello = next(record.frame.payload for record in records
                     if record.frame.frame_type == FrameType.HELLO)
        l1s = next(record.frame.payload for record in records
                   if record.frame.frame_type == FrameType.L1S_FRAME)

        def status(bridge, sent, generated):
            payload = bytearray(88)
            # 取得前の累積値 100/50 を含めても、窓内の 2 フレーム・L1S 1 件と閉じる。
            for offset, value in ((32, bridge), (36, bridge), (40, sent),
                                  (72, generated), (76, 51 if bridge > 100 else 50)):
                struct.pack_into("<I", payload, offset, value)
            return bytes(payload)

        for name, sent, generated, strict, adjusted in (
                ("closed", 102, 51, True, True),
                ("pending", 101, 51, False, True),
                ("mismatch", 102, 52, False, False)):
            with self.subTest(case=name), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "synthetic.hgraw"
                payloads = ((FrameType.HELLO, hello),
                            (FrameType.STATUS, status(100, 100, 50)),
                            (FrameType.L1S_FRAME, l1s),
                            (FrameType.STATUS, status(102, sent, generated)))
                with RawArchiveWriter(str(path)) as writer:
                    for sequence, (kind, payload) in enumerate(payloads):
                        writer.write(encode_frame(kind, sequence, sequence * 1_000_000, payload),
                                     records[0].pc_receive_ns + sequence * 1_000_000_000)
                report = validate_session(str(path))
                self.assertEqual(report["status"]["closed"], strict)
                self.assertEqual(report["zero_l1s_drop_proven"], strict)
                self.assertEqual(report["status"]["bridge_frames_observed_in_status_window"], 2)
                window = analyze_soak(str(path))["status"]["windows"][0]
                self.assertEqual(window["strict_snapshot_closed"], strict)
                self.assertEqual(window["closed"], adjusted)

    def test_importers_preserve_selected_wire_without_external_session(self):
        """同梱抜粋の再抽出を検証する。未公開の完全原セッションを再現したとは主張しない。"""
        from tools import import_bridge_session_fixture, import_24h_session_fixture
        for importer, source in (
                (import_bridge_session_fixture, SESSION / "selected_records.hgraw"),
                (import_24h_session_fixture, ROOT / "test/fixtures/bridge_24h_session/selected_records.hgraw")):
            with self.subTest(importer=importer.__name__):
                artifacts = importer.build(source)
                self.assertEqual(artifacts, importer.build(source))
                self.assertNotIn("README.md", artifacts)
                original = {(record.pc_receive_ns, record.wire)
                            for record in RawArchiveReader(str(source))}
                with tempfile.TemporaryDirectory() as directory:
                    selected = Path(directory) / "selected.hgraw"
                    selected.write_bytes(artifacts["selected_records.hgraw"])
                    reader = RawArchiveReader(str(selected))
                    extracted = [(record.pc_receive_ns, record.wire) for record in reader]
                self.assertTrue(extracted)
                self.assertTrue(set(extracted) <= original)
                self.assertEqual(reader.corrupt_records, 0)

    def test_real_session_integrity_and_coverage(self):
        with self.subTest(check="selected_archive_header_version"):
            reader = RawArchiveReader(str(SESSION / "selected_records.hgraw"))
            list(reader)
            self.assertEqual((reader.archive_major, reader.archive_minor), (1, 0))
        with self.subTest(check="selected_real_records_replay"):
            reader = RawArchiveReader(str(SESSION / "selected_records.hgraw"))
            records = list(reader)
            self.assertGreaterEqual(len(records), 10)
            self.assertEqual(reader.stats.archive_crc_errors, 0)
            kinds = {record.frame.frame_type for record in records}
            self.assertTrue({FrameType.HELLO, FrameType.STATUS,
                             FrameType.TIME_STATUS, FrameType.L1S_FRAME} <= kinds)
        with self.subTest(check="selected_real_mt_sv_pab_coverage"):
            values = [parse_payload(record.frame) for record in
                      RawArchiveReader(str(SESSION / "selected_records.hgraw"))
                      if record.frame.frame_type == FrameType.L1S_FRAME]
            self.assertTrue({43, 44, 47, 48, 49, 50} <=
                            {value.message_type for value in values})
            self.assertEqual({value.sv_id for value in values}, {3, 7})
            self.assertEqual({value.pab for value in values}, {0x53, 0x9A, 0xC6})
        with self.subTest(check="selected_time_status_is_typed_and_matched"):
            values = [parse_payload(record.frame) for record in
                      RawArchiveReader(str(SESSION / "selected_records.hgraw"))
                      if record.frame.frame_type == FrameType.TIME_STATUS]
            self.assertTrue(values)
            self.assertTrue(any(value.matched for value in values))
        with self.subTest(check="selected_contains_first_and_last_status_evidence"):
            values = [parse_payload(record.frame) for record in
                      RawArchiveReader(str(SESSION / "selected_records.hgraw"))
                      if record.frame.frame_type == FrameType.STATUS]
            self.assertGreaterEqual(len(values), 2)
            self.assertLess(values[0].bridge_generated, values[-1].bridge_generated)
        with self.subTest(check="selected_l1s_independent_verification"):
            for record in RawArchiveReader(str(SESSION / "selected_records.hgraw")):
                if record.frame.frame_type == FrameType.L1S_FRAME:
                    result = compare_claim(parse_payload(record.frame))
                    self.assertFalse(result.mismatch)
                    self.assertTrue(result.verified.valid_crc)
        with self.subTest(check="real_manifest_source_and_privacy"):
            manifest = json.loads((SESSION / "manifest.json").read_text())
            self.assertEqual(manifest["evidence_source"], "real_hardware")
            self.assertFalse(manifest["privacy"]["coordinates_retained"])
            self.assertTrue(manifest["expected_zero_drop_verdict"])
        with self.subTest(check="manifest_offsets_are_deterministic_and_ordered"):
            manifest = json.loads((SESSION / "manifest.json").read_text())
            offsets = [item["source_offset"] for item in manifest["selected_records"]]
            self.assertEqual(offsets, sorted(offsets))
            self.assertEqual(len(offsets), len(set(offsets)))
        with self.subTest(check="expected_summary_has_exact_real_status_delta"):
            expected = json.loads((SESSION / "expected_summary.json").read_text())
            self.assertEqual(expected["status_delta"]["l1s_generated"], 7333)
            self.assertEqual(expected["status_delta"]["l1s_sent"], 7333)
            self.assertEqual(expected["status_delta"]["l1s_dropped"], 0)


    def test_negative_vectors(self):
        with self.subTest(check="synthetic_negative_vectors_are_rejected"):
            vectors = json.loads((SESSION / "negative_vectors.json").read_text())
            self.assertEqual(vectors["source"], "synthetic_derived_from_real")
            items = {item["name"]: item for item in vectors["vectors"]}
            with self.assertRaises(CobsError):
                decode_wire(bytes.fromhex(items["cobs_corrupted"]["wire_hex"]))
            with self.assertRaises(CrcError):
                decode_wire(bytes.fromhex(items["bridge_crc_corrupted"]["wire_hex"]))
            ubx = decode_wire(bytes.fromhex(items["ubx_checksum_corrupted"]["wire_hex"]))
            self.assertTrue(compare_claim(parse_payload(ubx)).mismatch)
            l1s = decode_wire(bytes.fromhex(items["l1s_crc_corrupted"]["wire_hex"]))
            self.assertFalse(compare_claim(parse_payload(l1s)).verified.valid_crc)
            meta = decode_wire(bytes.fromhex(items["metadata_mismatch"]["wire_hex"]))
            self.assertTrue(compare_claim(parse_payload(meta)).mismatch)
        with self.subTest(check="negative_vector_names_and_provenance_are_complete"):
            values = json.loads((SESSION / "negative_vectors.json").read_text())
            self.assertEqual({item["name"] for item in values["vectors"]},
                             {"archive_crc_corrupted", "cobs_corrupted",
                              "bridge_crc_corrupted", "sequence_gap",
                              "ubx_checksum_corrupted", "l1s_crc_corrupted",
                              "metadata_mismatch"})
            self.assertEqual(values["source"], "synthetic_derived_from_real")

    def test_archive_crc_corruption_is_recoverably_rejected(self):
        values = json.loads((SESSION / "negative_vectors.json").read_text())
        raw = bytes.fromhex(next(item["record_hex"] for item in values["vectors"]
                                 if item["name"] == "archive_crc_corrupted"))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.hgraw"; path.write_bytes(raw)
            reader = RawArchiveReader(str(path))
            self.assertEqual(list(reader), [])
            self.assertEqual(reader.stats.archive_crc_errors, 1)

    def test_synthetic_sequence_gap_is_detected(self):
        values = json.loads((SESSION / "negative_vectors.json").read_text())
        gap = decode_wire(bytes.fromhex(next(item["wire_hex"] for item in values["vectors"]
                                                if item["name"] == "sequence_gap")))
        tracker = SequenceTracker(); tracker.observe(gap.sequence - 2)
        self.assertEqual(tracker.observe(gap.sequence).disposition,
                         SequenceDisposition.GAP)


SOAK_FIXTURE = ROOT / "test/fixtures/bridge_24h_session"
SOAK_SELECTED = SOAK_FIXTURE / "selected_records.hgraw"
SOAK_MANIFEST = json.loads((SOAK_FIXTURE / "manifest.json").read_text())
SOAK_EXPECTED = json.loads((SOAK_FIXTURE / "expected_summary.json").read_text())


class SoakTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.report = analyze_soak(str(SOAK_SELECTED), hourly_seconds=3600)

    def test_soak_provenance_and_source_summary(self):
        with self.subTest(check="fixture_source_is_real"):
            self.assertEqual(SOAK_MANIFEST["evidence_source"], "real_hardware")
        with self.subTest(check="fixture_records_source_firmware"):
            self.assertEqual(SOAK_MANIFEST["source_session"]["firmware"], "0.2.5")
        with self.subTest(check="fixture_protocol_is_one_zero"):
            self.assertEqual(SOAK_MANIFEST["source_session"]["bridge_protocol"], "1.0")
        with self.subTest(check="expected_summary_has_zero_drop_evidence"):
            self.assertTrue(SOAK_EXPECTED["zero_l1s_drop_proven"])
        with self.subTest(check="expected_summary_sequence_is_contiguous"):
            sequence = SOAK_EXPECTED["sequence"]
            self.assertEqual((sequence["gaps"], sequence["duplicates"],
                              sequence["out_of_order"]), (0, 0, 0))


    def test_fixture_does_not_claim_current_firmware_soak(self):
        text = (ROOT / "test/fixtures/README.md").read_text(encoding="utf-8")
        self.assertIn("現在のファームウェアの 24 時間実機試験完了を意味しません", text)

    def test_soak_selection_coverage(self):
        with self.subTest(check="fixture_contains_hourly_representatives"):
            reasons = [reason for item in SOAK_MANIFEST["selected_records"]
                       for reason in item["reasons"]]
            hours = [reason for reason in reasons if reason.startswith("hour ")]
            self.assertGreaterEqual(len(hours), 24)
        with self.subTest(check="fixture_contains_first_and_last_hello"):
            reasons = {reason for item in SOAK_MANIFEST["selected_records"]
                       for reason in item["reasons"]}
            self.assertIn("first HELLO", reasons)
            self.assertIn("last HELLO", reasons)
        with self.subTest(check="fixture_contains_first_and_last_status"):
            reasons = {reason for item in SOAK_MANIFEST["selected_records"]
                       for reason in item["reasons"]}
            self.assertIn("first STATUS", reasons)
            self.assertIn("last STATUS", reasons)
        with self.subTest(check="fixture_contains_first_and_last_time_status"):
            reasons = {reason for item in SOAK_MANIFEST["selected_records"]
                       for reason in item["reasons"]}
            self.assertIn("first TIME_STATUS", reasons)
            self.assertIn("last TIME_STATUS", reasons)
        with self.subTest(check="fixture_contains_every_observed_mt"):
            actual = {item.get("mt") for item in SOAK_MANIFEST["selected_records"]
                      if "mt" in item}
            self.assertTrue({43, 44, 47, 48, 49, 50} <= actual)
        with self.subTest(check="fixture_contains_every_observed_satellite"):
            actual = {item.get("sv_id") for item in SOAK_MANIFEST["selected_records"]
                      if "sv_id" in item}
            self.assertTrue({2, 3, 7} <= actual)
        with self.subTest(check="fixture_contains_all_pabs"):
            actual = {item.get("pab") for item in SOAK_MANIFEST["selected_records"]
                      if "pab" in item}
            self.assertTrue({"53", "9A", "C6"} <= actual)
        with self.subTest(check="fixture_contains_maximum_gap_boundaries"):
            reasons = {reason for item in SOAK_MANIFEST["selected_records"]
                       for reason in item["reasons"]}
            self.assertIn("before maximum L1S gap", reasons)
            self.assertIn("after maximum L1S gap", reasons)
        with self.subTest(check="fixture_contains_queue_watermark_changes"):
            reasons = [reason for item in SOAK_MANIFEST["selected_records"]
                       for reason in item["reasons"]]
            self.assertTrue(any("high-watermark became 4" in reason for reason in reasons))


    def test_soak_records_and_wire_hashes(self):
        with self.subTest(check="selected_archive_is_stream_replayable"):
            records = list(RawArchiveReader(str(SOAK_SELECTED)))
            self.assertEqual(len(records), len(SOAK_MANIFEST["selected_records"]))
        with self.subTest(check="selected_wire_hashes_match_manifest"):
            from hashlib import sha256
            records = list(RawArchiveReader(str(SOAK_SELECTED)))
            hashes = [sha256(record.wire).hexdigest() for record in records]
            self.assertEqual(hashes, [item["wire_sha256"]
                                      for item in SOAK_MANIFEST["selected_records"]])


    def test_soak_transport_and_time_analysis(self):
        with self.subTest(check="analyzer_streams_selected_records"):
            self.assertEqual(self.report["records"],
                             len(SOAK_MANIFEST["selected_records"]))
        with self.subTest(check="analyzer_hour_buckets_exist"):
            self.assertGreaterEqual(len(self.report["hourly"]), 24)
        with self.subTest(check="analyzer_reports_periodic_hello_without_epoch_reset"):
            self.assertTrue(self.report["hello"]["periodic_not_epoch"])
            self.assertEqual(self.report["sequence"]["epochs"], 1)
        with self.subTest(check="analyzer_checks_pc_timestamp_monotonicity"):
            self.assertEqual(self.report["timestamps"]["pc_nonmonotonic"], 0)
        with self.subTest(check="analyzer_reports_maximum_intervals"):
            self.assertGreater(self.report["timestamps"]["maximum_l1s_gap"]["seconds"], 0)
        with self.subTest(check="analyzer_reports_frame_byte_ratios"):
            ratios = self.report["throughput"]["frame_type_byte_ratio"]
            self.assertAlmostEqual(sum(ratios.values()), 1.0)


    def test_soak_semantic_analysis(self):
        with self.subTest(check="analyzer_reports_semantic_collision_metrics"):
            self.assertEqual(self.report["semantic"]["hash_collision_count"], 0)
            self.assertEqual(self.report["semantic"]["canonical_mismatch_count"], 0)
        with self.subTest(check="analyzer_keeps_null_out_of_events"):
            self.assertEqual(self.report["events"]["null_in_event_stream"], 0)
        with self.subTest(check="analyzer_keeps_slas_out_of_events"):
            self.assertEqual(self.report["events"]["slas_in_event_stream"], 0)
        with self.subTest(check="analyzer_extracts_dcr_rc_and_it_from_canonical_payload"):
            coverage = self.report["coverage"]
            self.assertTrue(coverage["dcr_rc"])
            self.assertTrue(coverage["dcr_it"])


    def test_audit_infers_semantic_mode_from_record_type(self):
        records = [record for record in RawArchiveReader(str(SOAK_SELECTED))
                   if record.frame.frame_type == FrameType.L1S_FRAME]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "misnamed-all.ndjson"
            path.write_text(json.dumps({
                "record_type": "semantic_observation", "service": "DCR",
                "receipt": {"bridge_sequence": records[0].frame.sequence, "mt": 43},
                "identity": {"semantic_fingerprint": "a", "frame_fingerprint": "b",
                             "event_key": "c"}}) + "\n")
            result = audit(str(path), str(SOAK_SELECTED))
        self.assertEqual(result["actual_stream_mode"], "semantic_observations")

    def test_audit_prefers_explicit_stream_mode(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "out.ndjson"
            path.write_text(json.dumps({"record_type": "session_start",
                                        "stream_mode": "events_only"}) + "\n")
            result = audit(str(path), str(SOAK_SELECTED))
        self.assertEqual(result["mode_evidence"], "explicit_metadata")

    def test_audit_detects_invalid_json(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "out.ndjson"
            path.write_text("{bad}\n")
            result = audit(str(path), str(SOAK_SELECTED))
        self.assertEqual(result["invalid_json_lines"], 1)

    def test_audit_detects_duplicate_output_lines(self):
        line = json.dumps({"record_type": "semantic_observation"}) + "\n"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "out.ndjson"
            path.write_text(line + line)
            result = audit(str(path), str(SOAK_SELECTED))
        self.assertEqual(result["duplicate_output_lines"], 1)

    def test_audit_reports_sequences_outside_archive(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "out.ndjson"
            path.write_text(json.dumps({"record_type": "frame_observation",
                                        "receipt": {"bridge_sequence": 0xFFFFFFFF}}) + "\n")
            result = audit(str(path), str(SOAK_SELECTED))
        self.assertEqual(result["output_sequences_outside_archive"], [0xFFFFFFFF])

if __name__ == "__main__":
    unittest.main()
