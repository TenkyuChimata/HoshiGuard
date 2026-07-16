from dataclasses import replace
from hashlib import sha256
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

from hoshiguard.bridge_frames import L1sFrame
from hoshiguard.bridge_protocol import BridgeFrame, FrameType, encode_frame
from hoshiguard.events.state_machine import EventProcessor
from hoshiguard.l1s import verify_ubx_l1s
from hoshiguard.operational import (DiskGuard, HealthReporter, PidFile,
                                    atomic_json, parse_duration, parse_quantity)
from hoshiguard.output.ndjson import NdjsonWriter
from hoshiguard.output.raw_archive import RawArchiveReader, RawArchiveWriter
from hoshiguard.routing import make_alert

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = json.loads((ROOT / "test/fixtures/real/manifest.json").read_text())


def alert():
    item = next(x for x in MANIFEST["fixtures"]
                if x["name"] == "qzss_l1s_mt43_pab53")
    verified = verify_ubx_l1s(bytes.fromhex(item["ubx_hex"]))
    claim = L1sFrame(1, verified.sfrbx.sv_id, verified.sfrbx.gnss_id,
                    verified.sfrbx.sig_id, verified.sfrbx.num_words,
                    verified.pab, verified.message_type, 0x0F,
                    verified.sfrbx.packet.raw, 4, 2, "0.2.7")
    frame = BridgeFrame(1, 0, FrameType.L1S_FRAME, 0, 1, 100, b"", 0)
    return make_alert(frame, claim, verified)


def event_result():
    return EventProcessor().observe(alert())


class V027OperationalTests(unittest.TestCase):
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
        self.assertEqual(value["producer_version"], "0.2.7")
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
            for item in files:
                sidecar = item.with_suffix(item.suffix + ".sha256")
                expected = sidecar.read_text().split()[0]
                self.assertEqual(expected, sha256(item.read_bytes()).hexdigest())

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

    def test_rotation_names_do_not_collide(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "out.ndjson"
            with NdjsonWriter(str(path), stream_mode="all_observations",
                              rotate_bytes=100) as writer:
                writer.write(event_result()); writer.write(event_result())
            names = [item.name for item in Path(directory).glob("*.ndjson")]
        self.assertEqual(len(names), len(set(names)))

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
        second = replace(first, identity=replace(
            first.identity, semantic_fingerprint="A" * 64),
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
            subprocess.run([sys.executable, "-m", "hoshiguard", "--bridge-file",
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
            subprocess.run([sys.executable, "-m", "hoshiguard", "--bridge-file",
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


if __name__ == "__main__":
    unittest.main()
