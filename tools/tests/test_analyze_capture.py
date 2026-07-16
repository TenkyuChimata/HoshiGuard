import contextlib
import io
import json
import pathlib
import tempfile
import unittest

import sys

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import analyze_capture
import replay_ubx


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


if __name__ == "__main__":
    unittest.main()
