import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from validate_dcr_test_scenario import validate

MANIFEST_PATH = ROOT / "test/fixtures/dcr_202607/scenario_manifest.json"
MANIFEST = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))


class DcrJulyScenarioManifestTests(unittest.TestCase):
    def test_manifest_schema_and_message_ids_are_unique(self):
        self.assertEqual(MANIFEST["schema"], "hoshiguard.dcr_official_scenario.v1")
        ids = [item["id"] for scenario in MANIFEST["scenarios"]
               for item in scenario["messages"]]
        self.assertEqual(len(ids), len(set(ids)))

    def test_oracle_is_not_binary_or_bit_spec(self):
        self.assertFalse(MANIFEST["binary_fixtures_fabricated"])
        self.assertTrue(MANIFEST["oracle"]["not_a_bit_layout_specification"])
        self.assertEqual(MANIFEST["oracle"]["bit_semantics_source"],
                         "IS-QZSS-DCR-016")

    def test_two_jst_and_utc_windows(self):
        windows = MANIFEST["broadcast_windows"]
        self.assertEqual(len(windows), 2)
        self.assertEqual(windows[0]["jst_start"], "2026-07-14T13:00:00+09:00")
        self.assertEqual(windows[0]["utc_start"], "2026-07-14T04:00:00Z")
        self.assertEqual(windows[1]["jst_start"], "2026-07-30T10:00:00+09:00")
        self.assertEqual(windows[1]["utc_start"], "2026-07-30T01:00:00Z")

    def test_rc_seven_and_preemption_semantics(self):
        self.assertEqual(MANIFEST["report_classification"]["Rc"], 7)
        self.assertTrue(MANIFEST["preemption"]["not_received_is_not_decoder_failure"])
        self.assertTrue(MANIFEST["preemption"]["highest_priority_actual_may_replace_test"])

    def test_seven_scenarios_nine_categories(self):
        self.assertEqual([item["id"] for item in MANIFEST["scenarios"]],
                         list(range(1, 8)))
        self.assertEqual({item["Dc"] for item in MANIFEST["disaster_categories"]},
                         {1, 2, 3, 4, 5, 8, 9, 10, 11})

    def test_manifest_contains_thirteen_expected_message_shapes(self):
        self.assertEqual(sum(len(item["messages"])
                             for item in MANIFEST["scenarios"]), 13)

    def test_all_messages_have_run_times_and_information_type(self):
        for scenario in MANIFEST["scenarios"]:
            for message in scenario["messages"]:
                self.assertEqual(set(message["At_utc_by_run"]), {"1", "2"})
                self.assertEqual(message["It"], 0)

    def test_scenario_intervals_include_two_five_minute_gaps(self):
        windows = {item["id"]: item["windows"][0]
                   for item in MANIFEST["scenarios"]}
        self.assertEqual(windows[4]["utc_start"], "2026-07-14T05:10:00Z")
        self.assertEqual(windows[3]["utc_end"], "2026-07-14T05:05:00Z")
        self.assertEqual(windows[6]["utc_start"], "2026-07-14T05:35:00Z")
        self.assertEqual(windows[5]["utc_end"], "2026-07-14T05:30:00Z")

    def test_tsunami_issue_update_clear(self):
        messages = {item["id"]: item for scenario in MANIFEST["scenarios"]
                    for item in scenario["messages"]}
        self.assertEqual(messages["1_tsunami_issue"]["action"], "issue")
        self.assertEqual(messages["2_tsunami_update"]["action"], "update")
        self.assertEqual(messages["3_tsunami_clear"]["action"], "clear")
        self.assertEqual(messages["3_tsunami_clear"]["expected"]["warning_code_raw"], 2)

    def test_lifecycle_groups_are_oracle_limited_not_global_ids(self):
        groups = {item["id"]: item for item in MANIFEST["lifecycle_groups"]}
        self.assertEqual(groups["official_scenario_tsunami"]["expected_actions"],
                         ["issue", "update", "clear"])
        self.assertEqual(groups["official_scenario_tsunami"]["identity_confidence"],
                         "official_scenario_only")

    def test_nankai_page_rules(self):
        item = next(item for scenario in MANIFEST["scenarios"]
                    for item in scenario["messages"] if item["id"] == "3_nankai")
        expected = item["expected"]
        self.assertEqual((expected["characters_per_page"], expected["expected_pages"]),
                         (6, 27))
        self.assertEqual(len(expected["text_utf8"]), 158)
        self.assertEqual(len(expected["text_utf8"].encode()), 474)
        self.assertFalse(expected["arrival_order_significant"])

    def test_eew_has_all_seventeen_forecast_region_bits(self):
        item = next(item for scenario in MANIFEST["scenarios"]
                    for item in scenario["messages"] if item["id"] == "1_eew")
        self.assertEqual(item["expected"]["forecast_region_bits_set"],
                         [37, 38, 39, 40, 42, 43, 44, 45, 46, 47, 48, 49,
                          50, 51, 66, 67, 68])

    def test_ash_has_five_parts_and_eighteen_ordered_locations(self):
        item = next(item for scenario in MANIFEST["scenarios"]
                    for item in scenario["messages"] if item["id"] == "5_ash_detail")
        entries = item["expected"]["entries_in_order"]
        self.assertEqual(item["expected_parts"], 5)
        self.assertEqual(len(entries), 18)
        self.assertEqual(sorted({entry["part"] for entry in entries}), [1, 2, 3, 4, 5])
        self.assertEqual([entry["part"] for entry in entries].count(5), 2)
        self.assertEqual(item["common_fields"]["warning_type_raw"], 2)

    def test_ash_uses_only_three_explicit_municipality_codes(self):
        item = next(item for scenario in MANIFEST["scenarios"]
                    for item in scenario["messages"] if item["id"] == "5_ash_detail")
        self.assertEqual({entry["municipality_raw"]
                          for entry in item["expected"]["entries_in_order"]},
                         {4321400, 4342800, 4343300})

    def test_weather_three_parts_fourteen_and_flood(self):
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

    def test_manifest_source_hash_matches_real_pdf(self):
        import hashlib
        self.assertEqual(hashlib.sha256((ROOT / "details_202607.pdf").read_bytes()).hexdigest(),
                         MANIFEST["oracle"]["sha256"])


if __name__ == "__main__":
    unittest.main()
