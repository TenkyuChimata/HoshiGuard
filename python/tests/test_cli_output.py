from contextlib import redirect_stderr
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from hoshiguard import cli as cli_module
from hoshiguard.bridge_protocol import FrameType
from hoshiguard.l1s import L1sError
from hoshiguard.output.raw_archive import RawArchiveReader, RawArchiveWriter

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = json.loads((ROOT / "test/fixtures/real/manifest.json").read_text())
REAL = next(item for item in MANIFEST["fixtures"] if item["name"] == "qzss_l1s_mt43_pab53")


class CliOutputTests(unittest.TestCase):
    def test_legacy_capture_to_human_and_ndjson(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory) / "capture.log"
            output = Path(directory) / "alerts.ndjson"
            capture.write_text(
                f"QZSS_L1S_UBX timestamp_us=123 pps_seq=4 svId=3 "
                f"numWords=9 pab=53 mt=43 crc=ok len=52 data={REAL['ubx_hex']}\n",
                encoding="ascii")
            completed = subprocess.run(
                [sys.executable, "-m", "hoshiguard", "--capture-log",
                 str(capture), "--human", "--ndjson", str(output),
                 "--no-session-records"],
                cwd=ROOT, text=True, capture_output=True, check=True)
            self.assertIn("[DCR]", completed.stdout)
            lines = output.read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(lines), 1)
            value = json.loads(lines[0])
            self.assertEqual(value["schema"], "hoshiguard.alert.v1")
            self.assertEqual(value["service"], "DCR")

    def test_malformed_legacy_record_does_not_crash(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory) / "capture.log"
            capture.write_bytes(b"garbage\xff\nUBX_RAW timestamp_us=1 len=8 data=NOTHEX\n")
            subprocess.run([sys.executable, "-m", "hoshiguard",
                            "--capture-log", str(capture), "--human"],
                           cwd=ROOT, check=True, capture_output=True)

    def test_semantic_error_isolated_after_raw_archive_write(self):
        fixture = ROOT / "test/fixtures/bridge_real_session/selected_records.hgraw"
        selected = [record for record in RawArchiveReader(str(fixture))
                    if record.frame.frame_type == FrameType.L1S_FRAME][:2]
        self.assertEqual(len(selected), 2)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.hgraw"
            archived = Path(directory) / "preserved.hgraw"
            ndjson = Path(directory) / "alerts.ndjson"
            report = Path(directory) / "report.json"
            health = Path(directory) / "health.json"
            with RawArchiveWriter(str(source)) as writer:
                for record in selected:
                    writer.write(record.wire, record.pc_receive_ns)

            real_make_alert = cli_module.make_alert
            calls = 0

            def fail_first(*args, **kwargs):
                nonlocal calls
                calls += 1
                if calls == 1:
                    raise L1sError("synthetic semantic failure")
                return real_make_alert(*args, **kwargs)

            stderr = io.StringIO()
            with patch("hoshiguard.cli.make_alert", side_effect=fail_first), \
                    redirect_stderr(stderr):
                result = cli_module.main([
                    "--bridge-file", str(source), "--all-observations",
                    "--raw-archive", str(archived), "--ndjson", str(ndjson),
                    "--json-report", str(report), "--health-file", str(health)])

            metrics = json.loads(report.read_text())
            state = json.loads(health.read_text())
            output = [json.loads(line) for line in ndjson.read_text().splitlines()]
            preserved = list(RawArchiveReader(str(archived)))

        self.assertEqual(result, 0)
        self.assertEqual(len(preserved), 2)
        self.assertEqual(metrics["verified_frame_observations"], 2)
        self.assertEqual(metrics["semantic_decode_errors"], 1)
        self.assertEqual(metrics["all_observations_emitted"], 1)
        self.assertEqual(metrics["input_to_output_accounting"]["unaccounted"], 0)
        self.assertEqual(sum(item.get("record_type") == "frame_observation"
                             for item in output), 1)
        self.assertIn("semantic_decode_error sequence=", stderr.getvalue())
        self.assertIn("service=", stderr.getvalue())
        self.assertEqual(state["overall_health"], "degraded")
        self.assertEqual(state["semantic_decode_errors"], 1)
        self.assertEqual(state["last_semantic_decode_error"]["exception"],
                         "L1sError")

    def test_unknown_programming_error_is_not_hidden(self):
        fixture = ROOT / "test/fixtures/bridge_real_session/selected_records.hgraw"
        with patch("hoshiguard.cli.make_alert",
                   side_effect=RuntimeError("programming defect")):
            with self.assertRaisesRegex(RuntimeError, "programming defect"):
                cli_module.main(["--bridge-file", str(fixture),
                                 "--all-observations", "--human"])


if __name__ == "__main__":
    unittest.main()
