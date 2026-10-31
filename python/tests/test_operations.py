"""現行 diagnose の出力、セッション・分割保存、ディスク方針と異常処理。"""
from support import ROOT, MANIFEST, alert, event_result, synthetic
from dataclasses import replace
from hashlib import sha256
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
from hoshiguard.bridge_protocol import FrameType, encode_frame
from hoshiguard.events.state_machine import EventProcessor
from hoshiguard.operational import (
    DiskGuard,
    HealthReporter,
    PidFile,
    atomic_json,
    parse_duration,
    parse_quantity,
)
from hoshiguard.output.ndjson import NdjsonWriter
from hoshiguard.output.raw_archive import RawArchiveReader, RawArchiveWriter
from contextlib import redirect_stderr
import io
from hoshiguard import diagnostics as cli_module
from hoshiguard.l1s import L1sError


class OperationalTests(unittest.TestCase):
    def test_parse_rotate_size_suffixes(self):
        self.assertEqual(parse_quantity("1K"), 1024)
        self.assertEqual(parse_quantity("1.5MiB"), 1572864)

    def test_parse_rotate_duration_suffixes(self):
        self.assertEqual(parse_duration("24h"), 86400)
        self.assertEqual(parse_duration("10m"), 600)

    def test_invalid_quantities_rejected(self):
        with self.assertRaises(ValueError):
            parse_quantity("forever")
        with self.assertRaises(ValueError):
            parse_duration("1week")

    def test_session_start_and_end_are_valid_ndjson(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "out.ndjson"
            with NdjsonWriter(str(path), stream_mode="all_observations",
                              session_records=True, session_id="fixed") as writer:
                writer.write(event_result())
            values = [json.loads(line) for line in path.read_text().splitlines()]
        self.assertEqual(values[0]["record_type"], "session_start")
        self.assertEqual(values[-1]["record_type"], "session_end")
        self.assertTrue(values[-1]["clean_shutdown"])

    def test_business_record_has_explicit_stream_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "out.ndjson"
            with NdjsonWriter(str(path), stream_mode="events_only",
                              session_records=True, session_id="fixed") as writer:
                writer.write(event_result())
            value = json.loads(path.read_text().splitlines()[1])
        self.assertEqual(value["stream_mode"], "events_only")
        from hoshiguard import __version__
        self.assertEqual(value["producer_version"], __version__)
        self.assertEqual(value["bridge_protocol_version"], "1.0")
        self.assertEqual(value["session_id"], "fixed")

    def test_abnormal_close_has_no_session_end(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "out.ndjson"
            try:
                with NdjsonWriter(str(path), stream_mode="semantic_observations",
                                  session_records=True):
                    raise RuntimeError("simulated crash")
            except RuntimeError:
                pass
            values = [json.loads(line) for line in path.read_text().splitlines()]
        self.assertEqual([x["record_type"] for x in values], ["session_start"])

    def test_compatibility_mode_omits_session_records(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "out.ndjson"
            with NdjsonWriter(str(path), stream_mode="all_observations") as writer:
                writer.write(event_result())
            values = path.read_text().splitlines()
        self.assertEqual(len(values), 1)

    def test_ndjson_size_rotation_writes_hash_sidecars(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "out.ndjson"
            with NdjsonWriter(str(path), stream_mode="all_observations",
                              session_records=True, rotate_bytes=1500) as writer:
                for _ in range(4):
                    writer.write(event_result())
            files = sorted(Path(directory).glob("*.ndjson"))
            hashes = sorted(Path(directory).glob("*.sha256"))
            self.assertGreater(len(files), 1)
            self.assertEqual(len(files), len(hashes))
            business_records = []
            for item in files:
                sidecar = item.with_suffix(item.suffix + ".sha256")
                expected = sidecar.read_text().split()[0]
                self.assertEqual(expected, sha256(item.read_bytes()).hexdigest())
                values = [json.loads(line) for line in item.read_text().splitlines()]
                business_records.extend(value for value in values
                                        if value["record_type"] not in ("session_start", "session_end"))
            # 名前の一意性だけでは上書きを検出できない。全出力の保存を確認する。
            self.assertEqual(len(business_records), 4)
            self.assertEqual({value["identity"]["semantic_fingerprint"]
                              for value in business_records},
                             {event_result().alert.identity.semantic_fingerprint})

    def test_ndjson_interval_rotation(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "out.ndjson"
            with NdjsonWriter(str(path), stream_mode="all_observations",
                              session_records=True, rotate_seconds=0.001) as writer:
                writer.write(event_result())
                time.sleep(0.003)
                writer.write(event_result())
            count = len(list(Path(directory).glob("*.ndjson")))
        self.assertGreaterEqual(count, 2)


    def test_raw_archive_size_rotation_is_replayable(self):
        wire = encode_frame(FrameType.HEARTBEAT, 1, 2, b"\0\0\0\0")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "one.hgraw"
            with RawArchiveWriter(str(path), rotate_bytes=80) as writer:
                for index in range(3):
                    writer.write(wire, index)
            archives = list(Path(directory).glob("*.hgraw"))
            count = sum(1 for item in archives for _ in RawArchiveReader(str(item)))
        self.assertEqual(count, 3)

    def test_raw_archive_hash_sidecars(self):
        wire = encode_frame(FrameType.HEARTBEAT, 1, 2, b"\0\0\0\0")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "one.hgraw"
            with RawArchiveWriter(str(path)) as writer:
                writer.write(wire, 1)
            sidecar = path.with_suffix(".hgraw.sha256")
            text = sidecar.read_text()
            self.assertIn("one.hgraw", text)
            self.assertEqual(text.split()[0], sha256(path.read_bytes()).hexdigest())

    def test_atomic_json_replaces_complete_document(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "health.json"
            atomic_json(path, {"value": 1})
            atomic_json(path, {"value": 2})
            value = json.loads(path.read_text())
            leftovers = list(Path(directory).glob("*.tmp"))
        self.assertEqual(value, {"value": 2})
        self.assertFalse(leftovers)

    def test_health_reporter_writes_runtime_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "health.json"
            reporter = HealthReporter(str(path), {"connected": True}, 0)
            reporter.update(force=True, last_sequence=9)
            value = json.loads(path.read_text())
        self.assertEqual(value["last_sequence"], 9)
        self.assertIn("process_uptime_seconds", value)

    def test_pid_file_is_exclusive_and_removed(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "run.pid"
            with PidFile(str(path)):
                self.assertTrue(path.exists())
                with self.assertRaises(FileExistsError):
                    with PidFile(str(path)):
                        pass
            self.assertFalse(path.exists())

    @staticmethod
    def _stat(free):
        return type("S", (), {"f_bavail": free, "f_frsize": 1})()

    def test_disk_ok_policy(self):
        with tempfile.TemporaryDirectory() as directory, patch(
                "hoshiguard.operational.os.statvfs", return_value=self._stat(1000)):
            result = DiskGuard(directory, 300, 200, 100).check()
        self.assertEqual(result.level, "ok")

    def test_disk_warning_keeps_both_outputs(self):
        with tempfile.TemporaryDirectory() as directory, patch(
                "hoshiguard.operational.os.statvfs", return_value=self._stat(250)):
            result = DiskGuard(directory, 300, 200, 100).check()
        self.assertTrue(result.allow_archive and result.allow_ndjson)

    def test_disk_critical_preserves_archive_first(self):
        with tempfile.TemporaryDirectory() as directory, patch(
                "hoshiguard.operational.os.statvfs", return_value=self._stat(150)):
            result = DiskGuard(directory, 300, 200, 100).check()
        self.assertTrue(result.allow_archive)
        self.assertFalse(result.allow_ndjson)

    def test_disk_critical_can_preserve_ndjson_instead(self):
        with tempfile.TemporaryDirectory() as directory, patch(
                "hoshiguard.operational.os.statvfs", return_value=self._stat(150)):
            result = DiskGuard(directory, 300, 200, 100,
                               critical_action="drop-archive").check()
        self.assertFalse(result.allow_archive)
        self.assertTrue(result.allow_ndjson)

    def test_disk_stop_disables_both_without_partial_write(self):
        with tempfile.TemporaryDirectory() as directory, patch(
                "hoshiguard.operational.os.statvfs", return_value=self._stat(50)):
            result = DiskGuard(directory, 300, 200, 100).check()
        self.assertFalse(result.allow_archive or result.allow_ndjson)

    def test_invalid_disk_threshold_order_rejected(self):
        with self.assertRaises(ValueError):
            DiskGuard(".", 100, 200, 50)

    def test_event_state_peak_is_bounded(self):
        processor = EventProcessor(capacity=1)
        first = alert()
        processor.observe(first)
        second = replace(synthetic(first, number=1),
                         receipt=replace(first.receipt, timestamp_us=200))
        processor.observe(second)
        self.assertEqual(processor.peak_semantic, 1)
        self.assertGreater(processor.capacity_evictions, 0)

    def test_ttl_eviction_is_reported_separately(self):
        processor = EventProcessor(ttl_us=1)
        first = alert(); processor.observe(first)
        processor.observe(replace(first, receipt=replace(first.receipt,
                                                         timestamp_us=200)))
        self.assertGreater(processor.ttl_evictions, 0)

    def test_all_observations_cli_accounting_has_no_unaccounted(self):
        fixture = ROOT / "test/fixtures/bridge_real_session/selected_records.hgraw"
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "all.ndjson"
            report = Path(directory) / "report.json"
            subprocess.run([sys.executable, "-m", "hoshiguard", "diagnose", "--bridge-file",
                            str(fixture), "--all-observations", "--ndjson",
                            str(output), "--json-report", str(report)],
                           cwd=ROOT, check=True, capture_output=True)
            value = json.loads(report.read_text())
        self.assertEqual(value["input_to_output_accounting"]["unaccounted"], 0)

    def test_all_observations_records_every_fixture_l1s(self):
        fixture = ROOT / "test/fixtures/bridge_real_session/selected_records.hgraw"
        expected = sum(record.frame.frame_type == FrameType.L1S_FRAME
                       for record in RawArchiveReader(str(fixture)))
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "all.ndjson"
            subprocess.run([sys.executable, "-m", "hoshiguard", "diagnose", "--bridge-file",
                            str(fixture), "--all-observations", "--ndjson", str(output)],
                           cwd=ROOT, check=True, capture_output=True)
            records = [json.loads(line) for line in output.read_text().splitlines()]
        self.assertEqual(sum(x.get("record_type") == "frame_observation"
                             for x in records), expected)

    def test_session_end_count_excludes_header_and_footer(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "out.ndjson"
            with NdjsonWriter(str(path), stream_mode="all_observations",
                              session_records=True) as writer:
                writer.write(event_result()); writer.write(event_result())
            end = json.loads(path.read_text().splitlines()[-1])
        self.assertEqual(end["objects_written"], 2)


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
                [sys.executable, "-m", "hoshiguard", "diagnose", "--capture-log",
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
            subprocess.run([sys.executable, "-m", "hoshiguard", "diagnose",
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
            with patch("hoshiguard.diagnostics.make_alert", side_effect=fail_first), \
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
        with patch("hoshiguard.diagnostics.make_alert",
                   side_effect=RuntimeError("programming defect")):
            with self.assertRaisesRegex(RuntimeError, "programming defect"):
                cli_module.main(["--bridge-file", str(fixture),
                                 "--all-observations", "--human"])

if __name__ == "__main__":
    unittest.main()
