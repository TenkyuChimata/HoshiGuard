import contextlib
import hashlib
import io
import json
import pathlib
import tempfile
import unittest

import sys

TOOLS = pathlib.Path(__file__).resolve().parents[1]
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))

import analyze_capture
import capture_io
import import_real_fixtures
import replay_ubx
import sanitize_capture


def ubx_packet(message_class, message_id, payload):
    body = bytes((message_class, message_id, len(payload) & 0xFF,
                  len(payload) >> 8)) + payload
    checksum = replay_ubx.ubx_checksum(body)
    return b"\xB5\x62" + body + bytes(checksum)


def raw_line(packet, timestamp=123):
    return (f"UBX_RAW timestamp_us={timestamp} len={len(packet)} "
            f"data={packet.hex().upper()}")


class V024ToolTests(unittest.TestCase):
    def test_binary_leading_garbage_is_counted_without_utf8_decode(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "capture.log"
            garbage = b"\xB0" + b"\xFC" * 31
            path.write_bytes(garbage + b"PPS timestamp_us=10 seq=1 interval_us=0\n")
            capture = capture_io.read_capture(path)
            self.assertEqual(capture.leading_garbage_bytes, len(garbage))
            self.assertEqual(capture.first_record_offset, len(garbage))
            self.assertEqual(len(capture.lines), 1)

    def test_compact_l1s_record_is_replayed_as_complete_ubx(self):
        manifest = json.loads((ROOT / "test/fixtures/real/manifest.json").read_text())
        item = next(value for value in manifest["fixtures"]
                    if value["name"] == "qzss_l1s_mt44_pabc6")
        packet = bytes.fromhex(item["ubx_hex"])
        line = (f"QZSS_L1S_UBX timestamp_us=1 pps_seq=2 svId={item['sv_id']} "
                f"numWords=9 pab=C6 mt=44 crc=ok len={len(packet)} "
                f"data={packet.hex().upper()}")
        records, errors = replay_ubx.parse_log([line])
        self.assertFalse(errors)
        self.assertEqual(records[0].packet, packet)
        self.assertEqual(replay_ubx.extract_l1s(records[0], {}).message_type, 44)

    def test_malformed_middle_record_does_not_hide_later_valid_record(self):
        packet = ubx_packet(0x0A, 0x04, b"")
        records, errors = replay_ubx.parse_log([
            raw_line(packet, 1),
            "UBX_RAW timestamp_us=2 len=99 data=XYZ",
            raw_line(packet, 3),
        ])
        self.assertEqual(len(records), 2)
        self.assertEqual(len(errors), 1)

    def test_analyzer_warns_when_nav_pvt_raw_is_present(self):
        packet = ubx_packet(0x01, 0x07, bytes(92))
        report = analyze_capture.analyze_lines([raw_line(packet)])
        self.assertTrue(report["privacy"]["nav_pvt_raw_present"])
        self.assertIn("coordinates", report["privacy"]["privacy_warning"])

    def test_sanitizer_removes_nav_pvt_but_keeps_qzss(self):
        nav = ubx_packet(0x01, 0x07, bytes(92))
        manifest = json.loads((ROOT / "test/fixtures/real/manifest.json").read_text())
        qzss = bytes.fromhex(manifest["fixtures"][0]["ubx_hex"])
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "capture.log"
            path.write_text(raw_line(nav) + "\n" + raw_line(qzss) + "\n")
            capture = capture_io.read_capture(path)
            clean, removed_nav, removed_coordinates = sanitize_capture.sanitize(
                capture, True)
        self.assertEqual(removed_nav, 1)
        self.assertEqual(removed_coordinates, 0)
        self.assertNotIn(nav.hex().upper().encode(), clean)
        self.assertIn(qzss.hex().upper().encode(), clean)

    def test_manifest_packet_hashes_and_checksums_match(self):
        manifest = json.loads((ROOT / "test/fixtures/real/manifest.json").read_text())
        for item in manifest["fixtures"]:
            packet = bytes.fromhex(item["ubx_hex"])
            self.assertEqual(hashlib.sha256(packet).hexdigest(), item["ubx_sha256"])
            error = replay_ubx.validate_ubx(packet)
            if item["expected_ubx_accept"]:
                self.assertIsNone(error, item["name"])
            else:
                self.assertIsNotNone(error, item["name"])

    def test_real_manifest_matches_available_capture(self):
        capture_path = ROOT / "capture-outdoor.log"
        if not capture_path.exists():
            self.skipTest("field capture intentionally not available")
        manifest = json.loads((ROOT / "test/fixtures/real/manifest.json").read_text())
        self.assertEqual(hashlib.sha256(capture_path.read_bytes()).hexdigest(),
                         manifest["source_capture_sha256"])
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(import_real_fixtures.main(
                [str(capture_path), "--check"]), 0)

    def test_public_capture_has_no_recoverable_nav_pvt_packet(self):
        public = ROOT / "capture-outdoor-public.log"
        if not public.exists():
            self.skipTest("sanitized capture not generated")
        capture = capture_io.read_capture(public)
        report = analyze_capture.analyze_lines(capture.lines, capture)
        self.assertFalse(report["privacy"]["nav_pvt_raw_present"])
        self.assertGreater(report["qzss"]["l1s"], 0)


if __name__ == "__main__":
    unittest.main()
