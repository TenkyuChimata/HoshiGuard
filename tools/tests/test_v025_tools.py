import json
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]


class V025ToolTests(unittest.TestCase):
    def run_tool(self, name):
        return subprocess.run([sys.executable, str(ROOT / "tools" / name)],
                              cwd=ROOT, text=True, capture_output=True, check=True)

    def test_spec_inventory(self):
        self.assertIn("spec inventory OK",
                      self.run_tool("check_spec_inventory.py").stdout)

    def test_traceability(self):
        self.assertIn("traceability OK",
                      self.run_tool("check_alert_traceability.py").stdout)

    def test_support_matrix(self):
        self.assertIn("support matrix OK",
                      self.run_tool("check_alert_support_matrix.py").stdout)

    def test_scenario_manifest_is_test_metadata_not_fabricated_binary(self):
        manifest = json.loads((ROOT / "test/fixtures/dcx_202607/scenario_manifest.json").read_text())
        self.assertFalse(manifest["binary_fixtures_fabricated"])
        self.assertEqual([item["id"] for item in manifest["scenarios"]],
                         ["1.1", "1.2", "2.1", "2.2", "3.1", "3.2"])
        self.assertEqual(manifest["required_coverage"]["A4"],
                         {"start": 1, "end": 113})
        self.assertEqual(manifest["required_coverage"]["extensions"],
                         list(range(1, 10)))


if __name__ == "__main__":
    unittest.main()
