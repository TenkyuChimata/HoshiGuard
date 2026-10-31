"""仕様一覧、公式シナリオの期待値と現行検証ツールの確認。"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(ROOT / "python"))
sys.path.insert(0, str(TOOLS))

import json
import subprocess
import unittest
import tempfile
from validate_dcr_test_scenario import validate


class SpecificationTests(unittest.TestCase):
    def test_dcx_scenario_validator_uses_bundled_reception(self):
        """実受信の一部を解析し、未観測の公式シナリオを受信済みとしない。"""
        from hoshiguard.bridge_frames import FrameType, parse_payload
        from hoshiguard.output.raw_archive import RawArchiveReader
        archive = ROOT / "test/fixtures/bridge_24h_session/selected_records.hgraw"
        expected = sum(parse_payload(record.frame).message_type == 44
                       for record in RawArchiveReader(str(archive))
                       if record.frame.frame_type == FrameType.L1S_FRAME)
        completed = subprocess.run(
            [sys.executable, str(ROOT / "tools/validate_dcx_test_scenario.py"),
             str(archive), "--manifest", str(ROOT / "test/fixtures/official/dcx-scenario.json")],
            cwd=ROOT, text=True, capture_output=True, check=True)
        report = json.loads(completed.stdout)
        self.assertGreater(expected, 0)
        self.assertEqual(report["decoded"], expected)
        self.assertEqual((report["decode_failed"], report["crc_failed"]), (0, 0))
        self.assertEqual(report["timing"]["inside_schedule"], 0)
        self.assertGreater(report["not_received"]["count"], 0)

    def run_tool(self, name):
        return subprocess.run([sys.executable, str(ROOT / "tools" / name)],
                              cwd=ROOT, text=True, capture_output=True, check=True)

    def test_repository_specification_checks(self):
        with self.subTest(check="spec_inventory"):
            self.assertIn("spec inventory OK",
                          self.run_tool("check_spec_inventory.py").stdout)
        with self.subTest(check="traceability"):
            self.assertIn("traceability OK",
                          self.run_tool("check_alert_traceability.py").stdout)
        with self.subTest(check="support_matrix"):
            self.assertIn("support matrix OK",
                          self.run_tool("check_alert_support_matrix.py").stdout)


    def test_scenario_manifest_is_test_metadata_not_fabricated_binary(self):
        manifest = json.loads((ROOT / "test/fixtures/official/dcx-scenario.json").read_text())
        self.assertFalse(manifest["binary_fixtures_fabricated"])
        self.assertEqual([item["id"] for item in manifest["scenarios"]],
                         ["1.1", "1.2", "2.1", "2.2", "3.1", "3.2"])
        self.assertEqual(manifest["required_coverage"]["A4"],
                         {"start": 1, "end": 113})
        self.assertEqual(manifest["required_coverage"]["extensions"],
                         list(range(1, 10)))


MANIFEST_PATH = ROOT / "test/fixtures/official/dcr-scenario.json"
MANIFEST = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))


class DcrScenarioTests(unittest.TestCase):
    def test_official_manifest_integrity(self):
        with self.subTest(check="manifest_schema_and_message_ids_are_unique"):
            self.assertEqual(MANIFEST["schema"], "hoshiguard.dcr_official_scenario.v1")
            ids = [item["id"] for scenario in MANIFEST["scenarios"]
                   for item in scenario["messages"]]
            self.assertEqual(len(ids), len(set(ids)))
        with self.subTest(check="oracle_is_not_binary_or_bit_spec"):
            self.assertFalse(MANIFEST["binary_fixtures_fabricated"])
            self.assertTrue(MANIFEST["oracle"]["not_a_bit_layout_specification"])
            self.assertEqual(MANIFEST["oracle"]["bit_semantics_source"],
                             "IS-QZSS-DCR-016")
        with self.subTest(check="two_jst_and_utc_windows"):
            windows = MANIFEST["broadcast_windows"]
            self.assertEqual(len(windows), 2)
            self.assertEqual(windows[0]["jst_start"], "2026-07-14T13:00:00+09:00")
            self.assertEqual(windows[0]["utc_start"], "2026-07-14T04:00:00Z")
            self.assertEqual(windows[1]["jst_start"], "2026-07-30T10:00:00+09:00")
            self.assertEqual(windows[1]["utc_start"], "2026-07-30T01:00:00Z")
        with self.subTest(check="rc_seven_and_preemption_semantics"):
            self.assertEqual(MANIFEST["report_classification"]["Rc"], 7)
            self.assertTrue(MANIFEST["preemption"]["not_received_is_not_decoder_failure"])
            self.assertTrue(MANIFEST["preemption"]["highest_priority_actual_may_replace_test"])
        with self.subTest(check="seven_scenarios_nine_categories"):
            self.assertEqual([item["id"] for item in MANIFEST["scenarios"]],
                             list(range(1, 8)))
            self.assertEqual({item["Dc"] for item in MANIFEST["disaster_categories"]},
                             {1, 2, 3, 4, 5, 8, 9, 10, 11})
        with self.subTest(check="manifest_contains_thirteen_expected_message_shapes"):
            self.assertEqual(sum(len(item["messages"])
                                 for item in MANIFEST["scenarios"]), 13)
        with self.subTest(check="all_messages_have_run_times_and_information_type"):
            for scenario in MANIFEST["scenarios"]:
                for message in scenario["messages"]:
                    self.assertEqual(set(message["At_utc_by_run"]), {"1", "2"})
                    self.assertEqual(message["It"], 0)
        with self.subTest(check="scenario_intervals_include_two_five_minute_gaps"):
            windows = {item["id"]: item["windows"][0]
                       for item in MANIFEST["scenarios"]}
            self.assertEqual(windows[4]["utc_start"], "2026-07-14T05:10:00Z")
            self.assertEqual(windows[3]["utc_end"], "2026-07-14T05:05:00Z")
            self.assertEqual(windows[6]["utc_start"], "2026-07-14T05:35:00Z")
            self.assertEqual(windows[5]["utc_end"], "2026-07-14T05:30:00Z")
        with self.subTest(check="manifest_source_matches_inventory"):
            inventory = json.loads((ROOT / "docs/spec-inventory.json").read_text(encoding="utf-8"))
            source = next(item for item in inventory["specs"]
                          if item.get("source_filename") == MANIFEST["oracle"]["source_file"])
            self.assertEqual(source["sha256"], MANIFEST["oracle"]["sha256"])
            self.assertFalse(source["repository_copy"])
            self.assertEqual(source["source"],
                             "https://qzss.go.jp/technical/dod/dc-report/test-data-distribution.html")


    def test_official_expected_payloads(self):
        with self.subTest(check="tsunami_issue_update_clear"):
            messages = {item["id"]: item for scenario in MANIFEST["scenarios"]
                        for item in scenario["messages"]}
            self.assertEqual(messages["1_tsunami_issue"]["action"], "issue")
            self.assertEqual(messages["2_tsunami_update"]["action"], "update")
            self.assertEqual(messages["3_tsunami_clear"]["action"], "clear")
            self.assertEqual(messages["3_tsunami_clear"]["expected"]["warning_code_raw"], 2)
        with self.subTest(check="lifecycle_groups_are_oracle_limited_not_global_ids"):
            groups = {item["id"]: item for item in MANIFEST["lifecycle_groups"]}
            self.assertEqual(groups["official_scenario_tsunami"]["expected_actions"],
                             ["issue", "update", "clear"])
            self.assertEqual(groups["official_scenario_tsunami"]["identity_confidence"],
                             "official_scenario_only")
        with self.subTest(check="nankai_page_rules"):
            item = next(item for scenario in MANIFEST["scenarios"]
                        for item in scenario["messages"] if item["id"] == "3_nankai")
            expected = item["expected"]
            self.assertEqual((expected["characters_per_page"], expected["expected_pages"]),
                             (6, 27))
            self.assertEqual(len(expected["text_utf8"]), 158)
            self.assertEqual(len(expected["text_utf8"].encode()), 474)
            self.assertFalse(expected["arrival_order_significant"])
        with self.subTest(check="eew_has_all_seventeen_forecast_region_bits"):
            item = next(item for scenario in MANIFEST["scenarios"]
                        for item in scenario["messages"] if item["id"] == "1_eew")
            self.assertEqual(item["expected"]["forecast_region_bits_set"],
                             [37, 38, 39, 40, 42, 43, 44, 45, 46, 47, 48, 49,
                              50, 51, 66, 67, 68])
        with self.subTest(check="ash_has_five_parts_and_eighteen_ordered_locations"):
            item = next(item for scenario in MANIFEST["scenarios"]
                        for item in scenario["messages"] if item["id"] == "5_ash_detail")
            entries = item["expected"]["entries_in_order"]
            self.assertEqual(item["expected_parts"], 5)
            self.assertEqual(len(entries), 18)
            self.assertEqual(sorted({entry["part"] for entry in entries}), [1, 2, 3, 4, 5])
            self.assertEqual([entry["part"] for entry in entries].count(5), 2)
            self.assertEqual(item["common_fields"]["warning_type_raw"], 2)
        with self.subTest(check="ash_uses_only_three_explicit_municipality_codes"):
            item = next(item for scenario in MANIFEST["scenarios"]
                        for item in scenario["messages"] if item["id"] == "5_ash_detail")
            self.assertEqual({entry["municipality_raw"]
                              for entry in item["expected"]["entries_in_order"]},
                             {4321400, 4342800, 4343300})
        with self.subTest(check="weather_three_parts_fourteen_and_flood"):
            messages = {item["id"]: item for scenario in MANIFEST["scenarios"]
                        for item in scenario["messages"]}
            weather = messages["6_weather"]
            self.assertEqual(weather["expected_parts"], 3)
            self.assertEqual(len(weather["expected"]["entries_in_order"]), 14)
            self.assertEqual(messages["6_flood_issue"]["expected"]["entries"][0]
                             ["warning_level_raw"], 2)
            self.assertEqual(messages["7_flood_clear"]["expected"]["entries"][0]
                             ["warning_level_raw"], 1)


    def test_validator_distinguishes_not_received_from_decode_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            empty = Path(directory) / "empty.ndjson"; empty.write_text("")
            report = validate(empty, MANIFEST, "ndjson")
            self.assertEqual(report["final_verdict"], "not_observed")
            self.assertEqual(report["decode_failures"], 0)
            self.assertTrue(all(item["status"] == "not_received"
                                for item in report["messages"]))
            malformed = Path(directory) / "bad.ndjson"
            malformed.write_text("{broken\n")
            failed = validate(malformed, MANIFEST, "ndjson")
            self.assertEqual(failed["final_verdict"], "fail")
            self.assertEqual(failed["decode_failures"], 1)

    def test_actual_messages_are_counted_separately(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "actual.ndjson"
            path.write_text(json.dumps({"service": "DCR", "status": "actual",
                                        "category": {"code": 14},
                                        "payload": {"fields": []},
                                        "validity": {"semantics": True,
                                                     "l1s_crc": True}}) + "\n")
            report = validate(path, MANIFEST, "ndjson")
            self.assertEqual(report["actual_messages_mixed_in"], 1)
            self.assertEqual(report["received_test_messages"], 0)


if __name__ == "__main__":
    unittest.main()
