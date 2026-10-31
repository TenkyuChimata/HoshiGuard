"""捕捉解析、プライバシー処理と fixture 生成・照合の検証。"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(ROOT / "python"))
sys.path.insert(0, str(TOOLS))

import contextlib
import io
import json
import pathlib
import tempfile
import unittest
from unittest.mock import patch
import re
import analyze_capture
import replay_ubx
import hashlib
import capture_io
import import_real_fixtures
import sanitize_capture


DCX_SAMPLE = bytes.fromhex(
    "C6B0600DE2BF88E92000000000000000"
    "00001FFFFFFFFFFFC0000011A1C84ED4"
)


def make_sfrbx_log(timestamp=1234567):
    words = b"".join(DCX_SAMPLE[index:index + 4][::-1]
                     for index in range(0, 32, 4))
    payload = bytes((5, 4, 1, 0, 8, 0, 2, 0)) + words
    body = bytes((2, 0x13, len(payload), 0)) + payload
    ck_a, ck_b = replay_ubx.ubx_checksum(body)
    packet = b"\xB5\x62" + body + bytes((ck_a, ck_b))
    return f"UBX_RAW timestamp_us={timestamp} len={len(packet)} data={packet.hex().upper()}\n", packet


class AnalyzeCaptureTests(unittest.TestCase):
    def test_legacy_lu_timestamp_is_rejected_as_malformed(self):
        records, errors = replay_ubx.parse_log([
            "UBX_RAW timestamp_us=lu len=1171398 data=B562\n"
        ])
        self.assertFalse(records)
        self.assertEqual(len(errors), 1)
        self.assertIn("malformed", errors[0])

    def test_interleaved_raw_fragment_is_not_silently_accepted(self):
        line, _ = make_sfrbx_log()
        split = line.index("data=") + 12
        fragments = [line[:split] + "\n", "CFG key=0x40520001\n",
                     line[split:]]
        records, errors = replay_ubx.parse_log(fragments)
        self.assertFalse(records)
        self.assertEqual(len(errors), 1)

    def test_basic_capture_summary(self):
        ubx, _ = make_sfrbx_log()
        lines = [
            "SFRBX_HDR timestamp_us=1234567 gnssId=5 svId=4 sigId=1 freqId=0 numWords=8\n",
            ubx,
            "PPS timestamp_us=1000000 seq=1 interval_us=0 lost=0 abnormal=0\n",
            "TIMTP timestamp_us=900000 previous_pps_seq=0 tow_ms=1000 tow_sub_ms=0 qerr_ps=1 week=2400 flags=03 ref_info=40 time_base=UTC utc_available=yes locked=yes qerr_valid=yes\n",
            "TIMTP_MATCH timestamp_us=1000000 pps_seq=1 matched=yes ambiguous=no quality=tentative reason=not_stable\n",
        ]
        report = analyze_capture.analyze_lines(lines)
        self.assertEqual(report["file"]["ubx_checksum_ok"], 1)
        self.assertEqual(report["gnss"]["QZSS"], 1)
        self.assertEqual(report["qzss"]["l1s"], 1)
        self.assertEqual(report["qzss"]["mt44"], 1)
        self.assertEqual(report["time"]["tentative"], 1)

    def test_json_report_command(self):
        ubx, _ = make_sfrbx_log()
        with tempfile.TemporaryDirectory() as directory:
            capture = pathlib.Path(directory) / "capture.log"
            output = pathlib.Path(directory) / "report.json"
            capture.write_text(ubx, encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()):
                result = analyze_capture.main([str(capture), "--json", str(output)])
            self.assertEqual(result, 0)
            report = json.loads(output.read_text(encoding="utf-8"))
            self.assertIn("file", report)
            self.assertIn("qzss", report)
            self.assertIn("time", report)

    def test_cpp_fixture_export_reimports_same_packet(self):
        line, packet = make_sfrbx_log()
        records, errors = replay_ubx.parse_log([line])
        self.assertFalse(errors)
        with tempfile.TemporaryDirectory() as directory:
            fixture = pathlib.Path(directory) / "captured.h"
            replay_ubx.export_cpp(fixture, records)
            text = fixture.read_text(encoding="utf-8")
            for value in packet:
                self.assertIn(f"0x{value:02X}", text)
        replayed = replay_ubx.extract_l1s(records[0], {})
        self.assertIsNotNone(replayed)
        self.assertEqual(replayed.raw, DCX_SAMPLE)

    def test_debug_and_self_test_pps_lines_are_not_capture_events(self):
        lines = [
            "PPS seq=1 interval_us=0\n",
            "PPS timestamp_us=123098 seq=1 interval_us=0 lost=0 abnormal=0\n",
            "PPS observed: YES\n",
        ]
        report = analyze_capture.analyze_lines(lines)
        self.assertEqual(report["file"]["pps"], 1)
        self.assertEqual(report["time"]["micros_wraps"], 0)


def ubx_packet(message_class, message_id, payload):
    body = bytes((message_class, message_id, len(payload) & 0xFF,
                  len(payload) >> 8)) + payload
    checksum = replay_ubx.ubx_checksum(body)
    return b"\xB5\x62" + body + bytes(checksum)


def raw_line(packet, timestamp=123):
    return (f"UBX_RAW timestamp_us={timestamp} len={len(packet)} "
            f"data={packet.hex().upper()}")


class CapturePrivacyTests(unittest.TestCase):
    def test_cpp_packets_match_manifest_and_generator(self):
        manifest = json.loads((ROOT / "test/fixtures/real/manifest.json").read_text())
        header = (ROOT / "test/fixtures/packets.h").read_text()
        items = []
        for item in manifest["fixtures"]:
            with self.subTest(fixture=item["name"]):
                symbol = "k" + "".join(part.capitalize() for part in item["name"].split("_"))
                body = re.search(r"\b" + symbol + r"\[\] = \{([^}]+)\}", header).group(1)
                actual = bytes(int(value, 16) for value in re.findall(r"0x([0-9A-F]{2})", body))
                self.assertEqual(actual, bytes.fromhex(item["ubx_hex"]))
                self.assertEqual(hashlib.sha256(actual).hexdigest(), item["ubx_sha256"])
                self.assertTrue((ROOT / "test/fixtures/real" / item["header"]).is_file())
                items.append((item["name"], actual, item["expected_ubx_accept"],
                              item["expected_l1s"], item["pab"] or 0,
                              item["message_type"] or 0,
                              item["num_words"] if item["expected_l1s"] else 0))
        self.assertEqual(header, import_real_fixtures.packet_header(items))

    def test_capture_import_and_check_without_external_log(self):
        """実 packet を再配置した合成ログで生成・照合経路を検証する。原ログ再現の証拠ではない。"""
        manifest = json.loads((ROOT / "test/fixtures/real/manifest.json").read_text())
        lines = [raw_line(bytes.fromhex(item["ubx_hex"]), item["timestamp_us"])
                 for item in manifest["fixtures"] if item["source"] == "real"]
        lines += ["PPS timestamp_us=1000000 seq=1 interval_us=0"]
        with tempfile.TemporaryDirectory() as directory:
            destination = pathlib.Path(directory)
            source = destination / "synthetic-capture.log"
            source.write_text("\n".join(lines) + "\n")
            packets = destination / "packets.h"
            with patch.multiple(import_real_fixtures, ROOT=destination,
                                REAL_DIR=destination / "real", PACKETS=packets,
                                ARRIVALS=destination / "real/arrivals.h"), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                args = [str(source), "--capture-date", "2026-07-14"]
                self.assertEqual(import_real_fixtures.main(args), 0)
                self.assertEqual(import_real_fixtures.main(args + ["--check"]), 0)
                generated = json.loads((destination / "real/manifest.json").read_text())
                self.assertEqual({item["name"] for item in generated["fixtures"]},
                                 {item["name"] for item in manifest["fixtures"]})
                packets.write_text(packets.read_text().replace("0xB5", "0xB4", 1))
                self.assertEqual(import_real_fixtures.main(args + ["--check"]), 1)

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

if __name__ == "__main__":
    unittest.main()
