import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

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


if __name__ == "__main__":
    unittest.main()
