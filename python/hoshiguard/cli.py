from __future__ import annotations

import argparse
from collections import Counter
from contextlib import ExitStack
from dataclasses import asdict, replace
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import signal
import sys
import time
import uuid

from . import __version__
from .bridge_frames import (Hello, L1sFrame as ClaimedL1sFrame, Status,
                            TimeStatus, parse_payload)
from .bridge_protocol import (
    BridgeFrame, FrameType, PROTOCOL_MAJOR, PROTOCOL_MINOR,
    SequenceDisposition, SequenceTracker,
)
from .events import EventProcessor
from .dcr.reassembly import NankaiTroughReassembler
from .l1s import compare_claim, verify_ubx_l1s
from .output import NdjsonWriter, RawArchiveReader, RawArchiveWriter, format_human
from .operational import (DiskGuard, HealthReporter, PidFile, parse_duration,
                          parse_quantity)
from .routing import make_alert
from .serial_receiver import SerialReceiver

CAPTURE_RE = re.compile(
    rb"(?:QZSS_L1S_UBX|UBX_RAW)[^\r\n]*timestamp_us=(\d+)[^\r\n]*data=([0-9A-Fa-f]+)"
)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog="hoshiguard", description="QZSS L1S Bridge and DCR/DCX decoder")
    source = result.add_mutually_exclusive_group(required=True)
    source.add_argument("--port", help="serial device for Bridge binary input")
    source.add_argument("--bridge-file", help="versioned .hgraw archive")
    source.add_argument("--capture-log", help="legacy text capture containing full UBX")
    result.add_argument("--baud", type=int, default=115200)
    result.add_argument("--human", action="store_true")
    result.add_argument("--language", choices=("en", "ja", "zh"), default="en")
    result.add_argument("--detail", action="store_true")
    result.add_argument("--ndjson", metavar="PATH", help="NDJSON path or - for stdout")
    result.add_argument("--omit-raw-json", action="store_true")
    result.add_argument("--raw-archive", metavar="PATH")
    result.add_argument("--rotate-size", type=parse_quantity, default=0,
                        help="rotate output after SIZE (for example 256M)")
    result.add_argument("--rotate-interval", type=parse_duration, default=0,
                        help="rotate output after duration (for example 24h)")
    result.add_argument("--output-directory")
    result.add_argument("--session-prefix", default="hoshiguard")
    result.add_argument("--session-id")
    result.add_argument("--fsync-interval", type=parse_duration, default=30.0)
    result.add_argument("--health-file")
    result.add_argument("--pid-file")
    result.add_argument("--no-session-records", action="store_true",
                        help="compatibility: omit NDJSON session_start/session_end")
    result.add_argument("--disk-warning-free", type=parse_quantity, default=1024**3)
    result.add_argument("--disk-critical-free", type=parse_quantity, default=512*1024**2)
    result.add_argument("--disk-stop-free", type=parse_quantity, default=128*1024**2)
    result.add_argument("--disk-critical-action",
                        choices=("drop-ndjson", "drop-archive", "stop"),
                        default="drop-ndjson")
    modes = result.add_mutually_exclusive_group()
    modes.add_argument("--all-observations", action="store_true",
                       help="emit every verified frame observation")
    modes.add_argument("--semantic-observations", action="store_true",
                       help="emit each canonical semantic observation once (default)")
    modes.add_argument("--events-only", action="store_true",
                       help="emit only issued/updated/cancelled/cleared lifecycle records")
    result.add_argument("--repeat-summary", action="store_true",
                        help="emit bounded aggregate summaries after replay")
    result.add_argument("--json-report", metavar="PATH",
                        help="write replay/output counters as JSON")
    result.add_argument("--show-repeats", action="store_true",
                        help=argparse.SUPPRESS)
    result.add_argument("--no-reconnect", action="store_true")
    return result


def _prepare_outputs(args, mode: str) -> str:
    session_id = args.session_id or str(uuid.uuid4())
    if args.output_directory:
        directory = Path(args.output_directory)
        directory.mkdir(parents=True, exist_ok=True)
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
        base = f"{args.session_prefix}-{stamp}"
        if not args.raw_archive:
            args.raw_archive = str(directory / f"{base}.hgraw")
        if not args.ndjson:
            args.ndjson = str(directory / f"{base}-{mode}.ndjson")
    return session_id


def _legacy_frames(path: str):
    sequence = 0
    data = Path(path).read_bytes()
    for match in CAPTURE_RE.finditer(data):
        try:
            ubx = bytes.fromhex(match.group(2).decode("ascii"))
            verified = verify_ubx_l1s(ubx)
        except ValueError:
            continue
        timestamp = int(match.group(1))
        flags = 0x0F if verified.valid_pab and verified.valid_crc else 0x03
        claim = ClaimedL1sFrame(0, verified.sfrbx.sv_id,
                                verified.sfrbx.gnss_id, verified.sfrbx.sig_id,
                                verified.sfrbx.num_words, verified.pab,
                                verified.message_type, flags, ubx, 0, 0,
                                "legacy-capture")
        frame = BridgeFrame(PROTOCOL_MAJOR, PROTOCOL_MINOR,
                            FrameType.L1S_FRAME, 0, sequence, timestamp, b"", 0)
        sequence = (sequence + 1) & 0xFFFFFFFF
        yield frame, claim, verified, False, time.time_ns()


def _bridge_frames(args, stop):
    if args.bridge_file:
        reader = RawArchiveReader(args.bridge_file)
        sequence = SequenceTracker()
        for record in reader:
            result = sequence.observe(
                record.frame.sequence,
                hello=record.frame.frame_type == FrameType.HELLO)
            if result.disposition == SequenceDisposition.GAP:
                print(f"bridge_sequence_gap missing={result.missing} "
                      f"sequence={record.frame.sequence} epoch={result.epoch}",
                      file=sys.stderr)
            elif result.disposition == SequenceDisposition.DUPLICATE:
                print(f"bridge_duplicate_sequence sequence={record.frame.sequence} "
                      f"epoch={result.epoch}", file=sys.stderr)
            elif result.disposition == SequenceDisposition.OUT_OF_ORDER:
                print(f"bridge_out_of_order sequence={record.frame.sequence} "
                      f"epoch={result.epoch}", file=sys.stderr)
            yield record.frame, record.pc_receive_ns
        if reader.corrupt_records:
            print(f"warning: recovered past {reader.corrupt_records} corrupt archive record(s)",
                  file=sys.stderr)
    else:
        receiver = SerialReceiver(args.port, args.baud,
                                  reconnect=not args.no_reconnect,
                                  status_callback=lambda message: print(
                                      message, file=sys.stderr, flush=True))
        yield from receiver.frames(stop)


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    mode = ("all" if args.all_observations or args.show_repeats else
            "events" if args.events_only else "semantic")
    stream_mode = {"all": "all_observations",
                   "semantic": "semantic_observations",
                   "events": "events_only"}[mode]
    session_id = _prepare_outputs(args, stream_mode)
    if not args.human and not args.ndjson and not args.raw_archive:
        args.human = True
    stopped = False

    def stop_handler(_signum, _frame):
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGINT, stop_handler)
    signal.signal(signal.SIGTERM, stop_handler)
    events = EventProcessor()
    dcr_reassembler = NankaiTroughReassembler()
    mismatches = 0
    semantic_dropped = 0
    metrics = {"received_l1s_frames": 0,
               "verified_frame_observations": 0,
               "all_observations_emitted": 0,
               "semantic_observations_emitted": 0,
               "event_lifecycle_emitted": 0,
               "repeat_summaries_emitted": 0,
               "bridge_verification_mismatches": 0,
               "semantic_alert_dropped": 0,
               "service_distribution": Counter(),
               "mt_distribution": Counter(),
               "record_type_distribution": Counter()}
    source_name = args.bridge_file or args.capture_log or args.port or "unknown"
    health = (HealthReporter(args.health_file, {
        "schema": "hoshiguard.health.v1", "producer_version": __version__,
        "connected": bool(args.bridge_file or args.capture_log),
        "serial_port": args.port, "current_epoch": 0,
        "sequence_gaps": 0, "bridge_crc_errors": 0, "ubx_errors": 0,
        "l1s_crc_errors": 0, "verification_mismatches": 0,
        "bridge_dropped": 0, "l1s_dropped": 0, "queue_high_watermark": 0,
        "overall_health": "starting", "session_id": session_id,
    }) if args.health_file else None)
    disk_path = (Path(args.output_directory) if args.output_directory else
                 Path(args.raw_archive or args.ndjson or ".").parent)
    disk = DiskGuard(disk_path, args.disk_warning_free,
                     args.disk_critical_free, args.disk_stop_free,
                     args.disk_critical_action)
    health_sequence = SequenceTracker()
    health_gaps = 0
    with PidFile(args.pid_file), ExitStack() as stack:
        ndjson = (stack.enter_context(NdjsonWriter(
            args.ndjson, include_raw=not args.omit_raw_json,
            stream_mode=stream_mode, producer_version=__version__,
            session_id=session_id, source=source_name,
            session_records=not args.no_session_records,
            rotate_bytes=args.rotate_size, rotate_seconds=args.rotate_interval,
            fsync_interval=args.fsync_interval))
            if args.ndjson else None)
        archive = (stack.enter_context(RawArchiveWriter(
            args.raw_archive, rotate_bytes=args.rotate_size,
            rotate_seconds=args.rotate_interval,
            fsync_interval=args.fsync_interval))
                   if args.raw_archive else None)

        def emit(frame: BridgeFrame, claim: ClaimedL1sFrame, verified,
                 bridge_crc: bool) -> None:
            nonlocal semantic_dropped
            alert = make_alert(frame, claim, verified, bridge_crc=bridge_crc,
                               dcr_reassembler=dcr_reassembler)
            result = events.observe(alert)
            metrics["verified_frame_observations"] += 1
            metrics["service_distribution"][alert.service.value] += 1
            metrics["mt_distribution"][str(alert.receipt.mt)] += 1
            if mode == "semantic" and result.disposition == "repeated":
                return
            if mode == "events" and result.disposition not in (
                    "issued", "updated", "cancelled", "cleared",
                    "part_added", "completed"):
                return
            if mode == "all":
                result = replace(result, record_type="frame_observation")
                metrics["all_observations_emitted"] += 1
            elif mode == "semantic":
                result = replace(result, record_type="semantic_observation")
                metrics["semantic_observations_emitted"] += 1
            else:
                result = replace(result, record_type="event_lifecycle")
                metrics["event_lifecycle_emitted"] += 1
            metrics["record_type_distribution"][result.record_type] += 1
            if args.human:
                print(format_human(result, args.language, args.detail), flush=True)
            if ndjson:
                ndjson.write(result)

        if args.capture_log:
            for frame, claim, verified, bridge_crc, _pc_ns in _legacy_frames(args.capture_log):
                emit(frame, claim, verified, bridge_crc)
        else:
            for frame, pc_ns in _bridge_frames(args, lambda: stopped):
                health_result = health_sequence.observe(
                    frame.sequence, hello=frame.frame_type == FrameType.HELLO)
                health_gaps += health_result.missing
                if health:
                    health.update(last_sequence=frame.sequence,
                                  current_epoch=health_result.epoch,
                                  sequence_gaps=health_gaps,
                                  connected=True,
                                  current_archive_path=(str(archive.path)
                                                        if archive else None),
                                  current_ndjson_path=(str(ndjson.path)
                                                       if ndjson else None))
                decision = disk.check()
                if health:
                    health.update(disk_free=decision.free_bytes,
                                  disk_state=decision.level)
                if not decision.allow_ndjson and ndjson is not None:
                    ndjson.close(clean=False)
                    ndjson = None
                    print("disk_critical derived NDJSON disabled; raw archive retained",
                          file=sys.stderr, flush=True)
                if not decision.allow_archive and archive is not None:
                    print("disk_stop raw archive cannot be safely continued",
                          file=sys.stderr, flush=True)
                    stopped = True
                    break
                if archive and frame.wire:
                    archive.write(frame.wire, pc_ns)
                    if health:
                        health.update(bytes_written=archive.stream.tell())
                if frame.frame_type in (FrameType.HELLO, FrameType.STATUS,
                                        FrameType.TIME_STATUS):
                    try:
                        typed_status = parse_payload(frame)
                    except ValueError:
                        typed_status = None
                    if health and isinstance(typed_status, Hello):
                        health.update(last_hello=pc_ns, firmware=typed_status.firmware,
                                      protocol=(f"{typed_status.protocol_major}."
                                                f"{typed_status.protocol_minor}"))
                    elif health and isinstance(typed_status, Status):
                        health.update(last_status_time=pc_ns,
                                      bridge_dropped=typed_status.bridge_dropped,
                                      l1s_dropped=typed_status.l1s_dropped,
                                      queue_high_watermark=typed_status.queue_high_watermark,
                                      pps_state=typed_status.pps_state,
                                      time_quality=typed_status.time_quality,
                                      ubx_errors=typed_status.ubx_checksum_errors)
                if frame.frame_type != FrameType.L1S_FRAME:
                    continue
                metrics["received_l1s_frames"] += 1
                try:
                    claim = parse_payload(frame)
                    comparison = compare_claim(claim)
                except ValueError as exc:
                    mismatches += 1
                    metrics["bridge_verification_mismatches"] += 1
                    print(f"bridge_verification_mismatch: {exc}", file=sys.stderr)
                    if health:
                        health.update(verification_mismatches=mismatches,
                                      overall_health="degraded")
                    continue
                if comparison.mismatch or comparison.verified is None:
                    mismatches += 1
                    metrics["bridge_verification_mismatches"] += 1
                    print("bridge_verification_mismatch: " +
                          "; ".join(comparison.reasons), file=sys.stderr)
                    if health:
                        health.update(verification_mismatches=mismatches,
                                      overall_health="degraded")
                    continue
                emit(frame, claim, comparison.verified, True)
                if health:
                    health.update(last_l1s_time=pc_ns,
                                  overall_health="healthy",
                                  ndjson_bytes_written=(ndjson.stream.tell()
                                      if ndjson and ndjson.destination != "-" else None))
        if args.repeat_summary:
            for summary in events.repeat_summaries():
                if mode == "events" and not summary.alert:
                    continue
                metrics["repeat_summaries_emitted"] += 1
                metrics["record_type_distribution"]["service_summary"] += 1
                if args.human:
                    print(f"REPEAT service={summary.service} "
                          f"observation={summary.observation} "
                          f"count={summary.repeat_count} "
                          f"satellites={list(summary.satellites_seen)} "
                          f"pabs={[f'{value:02X}' for value in summary.pabs_seen]}",
                          flush=True)
                if ndjson:
                    ndjson.write_repeat_summary(summary)
    metrics["semantic_alert_dropped"] = semantic_dropped
    metrics["semantic_identity_evictions"] = events.evictions
    metrics["semantic_active_peak"] = events.peak_semantic
    metrics["event_active_peak"] = events.peak_events
    metrics["ttl_evictions"] = events.ttl_evictions
    metrics["capacity_evictions"] = events.capacity_evictions
    metrics["output_mode"] = stream_mode
    metrics["session_id"] = session_id
    metrics["input_to_output_accounting"] = {
        "l1s_frames": metrics["received_l1s_frames"],
        "verified": metrics["verified_frame_observations"],
        "output_objects": metrics["all_observations_emitted"] if mode == "all" else None,
        "filtered": metrics["bridge_verification_mismatches"],
        "unaccounted": (metrics["verified_frame_observations"] -
                        metrics["all_observations_emitted"] if mode == "all" else 0),
    }
    for name in ("service_distribution", "mt_distribution",
                 "record_type_distribution"):
        metrics[name] = dict(sorted(metrics[name].items()))
    if health:
        health.update(force=True, connected=False, overall_health="stopped",
                      verification_mismatches=mismatches)
    if args.json_report:
        path = Path(args.json_report)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(metrics, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
    if mismatches or semantic_dropped:
        print(f"summary mismatches={mismatches} semantic_alert_dropped={semantic_dropped}",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
