import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "python"))

from hoshiguard.bridge_frames import FrameType, parse_payload
from hoshiguard.bridge_protocol import (
    CobsError, CrcError, SequenceDisposition, SequenceTracker, decode_wire,
)
from hoshiguard.l1s import compare_claim
from hoshiguard.output.raw_archive import RawArchiveReader
from hoshiguard.session_validation import validate_session

FIXTURE = ROOT / "test/fixtures/bridge_real_session"


class RealBridgeSessionTests(unittest.TestCase):
    def test_selected_archive_header_version(self):
        reader = RawArchiveReader(str(FIXTURE / "selected_records.hgraw"))
        list(reader)
        self.assertEqual((reader.archive_major, reader.archive_minor), (1, 0))

    def test_selected_real_records_replay(self):
        reader = RawArchiveReader(str(FIXTURE / "selected_records.hgraw"))
        records = list(reader)
        self.assertGreaterEqual(len(records), 10)
        self.assertEqual(reader.stats.archive_crc_errors, 0)
        kinds = {record.frame.frame_type for record in records}
        self.assertTrue({FrameType.HELLO, FrameType.STATUS,
                         FrameType.TIME_STATUS, FrameType.L1S_FRAME} <= kinds)

    def test_selected_real_mt_sv_pab_coverage(self):
        values = [parse_payload(record.frame) for record in
                  RawArchiveReader(str(FIXTURE / "selected_records.hgraw"))
                  if record.frame.frame_type == FrameType.L1S_FRAME]
        self.assertTrue({43, 44, 47, 48, 49, 50} <=
                        {value.message_type for value in values})
        self.assertEqual({value.sv_id for value in values}, {3, 7})
        self.assertEqual({value.pab for value in values}, {0x53, 0x9A, 0xC6})

    def test_selected_time_status_is_typed_and_matched(self):
        values = [parse_payload(record.frame) for record in
                  RawArchiveReader(str(FIXTURE / "selected_records.hgraw"))
                  if record.frame.frame_type == FrameType.TIME_STATUS]
        self.assertTrue(values)
        self.assertTrue(any(value.matched for value in values))

    def test_selected_contains_first_and_last_status_evidence(self):
        values = [parse_payload(record.frame) for record in
                  RawArchiveReader(str(FIXTURE / "selected_records.hgraw"))
                  if record.frame.frame_type == FrameType.STATUS]
        self.assertGreaterEqual(len(values), 2)
        self.assertLess(values[0].bridge_generated, values[-1].bridge_generated)

    def test_selected_l1s_independent_verification(self):
        for record in RawArchiveReader(str(FIXTURE / "selected_records.hgraw")):
            if record.frame.frame_type == FrameType.L1S_FRAME:
                result = compare_claim(parse_payload(record.frame))
                self.assertFalse(result.mismatch)
                self.assertTrue(result.verified.valid_crc)

    def test_synthetic_negative_vectors_are_rejected(self):
        vectors = json.loads((FIXTURE / "negative_vectors.json").read_text())
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

    def test_archive_crc_corruption_is_recoverably_rejected(self):
        values = json.loads((FIXTURE / "negative_vectors.json").read_text())
        raw = bytes.fromhex(next(item["record_hex"] for item in values["vectors"]
                                 if item["name"] == "archive_crc_corrupted"))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.hgraw"; path.write_bytes(raw)
            reader = RawArchiveReader(str(path))
            self.assertEqual(list(reader), [])
            self.assertEqual(reader.stats.archive_crc_errors, 1)

    def test_synthetic_sequence_gap_is_detected(self):
        values = json.loads((FIXTURE / "negative_vectors.json").read_text())
        gap = decode_wire(bytes.fromhex(next(item["wire_hex"] for item in values["vectors"]
                                                if item["name"] == "sequence_gap")))
        tracker = SequenceTracker(); tracker.observe(gap.sequence - 2)
        self.assertEqual(tracker.observe(gap.sequence).disposition,
                         SequenceDisposition.GAP)

    def test_real_manifest_source_and_privacy(self):
        manifest = json.loads((FIXTURE / "manifest.json").read_text())
        self.assertEqual(manifest["evidence_source"], "real_hardware")
        self.assertFalse(manifest["privacy"]["coordinates_retained"])
        self.assertTrue(manifest["expected_zero_drop_verdict"])

    def test_manifest_offsets_are_deterministic_and_ordered(self):
        manifest = json.loads((FIXTURE / "manifest.json").read_text())
        offsets = [item["source_offset"] for item in manifest["selected_records"]]
        self.assertEqual(offsets, sorted(offsets))
        self.assertEqual(len(offsets), len(set(offsets)))

    def test_negative_vector_names_and_provenance_are_complete(self):
        values = json.loads((FIXTURE / "negative_vectors.json").read_text())
        self.assertEqual({item["name"] for item in values["vectors"]},
                         {"archive_crc_corrupted", "cobs_corrupted",
                          "bridge_crc_corrupted", "sequence_gap",
                          "ubx_checksum_corrupted", "l1s_crc_corrupted",
                          "metadata_mismatch"})
        self.assertEqual(values["source"], "synthetic_derived_from_real")

    def test_expected_summary_has_exact_real_status_delta(self):
        expected = json.loads((FIXTURE / "expected_summary.json").read_text())
        self.assertEqual(expected["status_delta"]["l1s_generated"], 7333)
        self.assertEqual(expected["status_delta"]["l1s_sent"], 7333)
        self.assertEqual(expected["status_delta"]["l1s_dropped"], 0)

    @unittest.skipUnless((ROOT / "v025-session.hgraw").exists(),
                         "full real session intentionally not committed")
    def test_full_real_session_closure_and_distribution(self):
        report = validate_session(str(ROOT / "v025-session.hgraw"))
        expected = json.loads((FIXTURE / "expected_summary.json").read_text())
        self.assertEqual(report["source"]["sha256"], expected["source_sha256"])
        self.assertEqual(report["frame_type_distribution"],
                         expected["frame_type_distribution"])
        self.assertEqual(report["l1s"]["mt_distribution"],
                         expected["l1s"]["mt_distribution"])
        self.assertEqual(report["sequence"]["gaps"], 0)
        self.assertTrue(report["status"]["closed"])
        self.assertTrue(report["zero_l1s_drop_proven"])

    @unittest.skipUnless((ROOT / "v025-session.hgraw").exists(),
                         "full real session intentionally not committed")
    def test_fixture_import_check_is_reproducible(self):
        completed = subprocess.run(
            [sys.executable, "tools/import_bridge_session_fixture.py",
             "v025-session.hgraw", "--check"], cwd=ROOT, text=True,
            capture_output=True, check=True)
        self.assertIn("check OK", completed.stdout)


if __name__ == "__main__":
    unittest.main()
