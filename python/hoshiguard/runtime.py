"""単一の同期受信・デコード処理。シリアルスレッドではネットワーク処理を行わない。"""
from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
import logging
from pathlib import Path
import threading
import time

from .bridge_frames import ErrorFrame, Status, parse_payload
from .bridge_protocol import BridgeFrame, FrameType, SequenceDisposition, SequenceTracker
from .dcr.decoder import DcrDecodeError
from .dcr.reassembly import NankaiTroughReassembler
from .dcx.decoder import DcxDecodeError
from .events import EventProcessor
from .l1s import L1sError, compare_claim
from .operational import DiskGuard
from .output.raw_archive import RawArchiveReader, RawArchiveWriter
from .publisher import AlertStore
from .routing import make_alert
from .serial_receiver import SerialReceiver

_LOG = logging.getLogger(__name__)


@dataclass(frozen=True)
class ReceiverConfig:
    port: str | None = None
    baud: int = 115200
    replay: str | None = None
    raw_archive: str | None = None
    rotate_bytes: int = 256 * 1024 * 1024
    dedup_capacity: int = 4096
    dedup_ttl_seconds: float = 7 * 86400


class ReceiverPipeline:
    def __init__(self, store: AlertStore, *, dedup_capacity: int = 4096,
                 dedup_ttl_seconds: float = 7 * 86400):
        self.store = store
        self.events = EventProcessor(capacity=dedup_capacity,
                                     ttl_us=int(dedup_ttl_seconds * 1_000_000),
                                     maximum_frame_fingerprints=0, track_evidence=False)
        self.reassembler = NankaiTroughReassembler()
        self.sequence = SequenceTracker()
        self.previous_status = None

    def process(self, frame: BridgeFrame, received_ns: int):
        sequence = self.sequence.observe(frame.sequence, hello=frame.frame_type == FrameType.HELLO)
        if sequence.missing:
            self.store.error("sequence_gaps", count=sequence.missing)
        self.store.frame_received(received_ns)
        if sequence.disposition == SequenceDisposition.NEW_EPOCH:
            self.reassembler = NankaiTroughReassembler()
            self.previous_status = None
        if frame.frame_type != FrameType.L1S_FRAME:
            try:
                status = parse_payload(frame)
                if isinstance(status, ErrorFrame):
                    self.store.error("upstream_errors")
                elif isinstance(status, Status):
                    previous = self.previous_status
                    for names, counter in ((("bridge_dropped",), "upstream_drops"),
                                           (("l1s_dropped",), "upstream_l1s_drops"),
                                           (("ubx_checksum_errors", "encode_errors"), "upstream_errors")):
                        delta = sum(max(0, getattr(status, name) - (getattr(previous, name) if previous else 0)) for name in names)
                        if delta:
                            self.store.error(counter, count=delta)
                    self.previous_status = status
            except ValueError:
                self.store.error("bridge_errors")
            return None
        try:
            claim = parse_payload(frame)
            checked = compare_claim(claim)
            if checked.mismatch or checked.verified is None or not checked.verified.valid_crc or not checked.verified.valid_pab:
                self.store.error("verification_errors")
                return None
        except ValueError:
            self.store.error("verification_errors")
            return None
        self.store.frame_received(received_ns, l1s=True)
        if checked.verified.message_type not in (43, 44):
            return None
        try:
            alert = make_alert(frame, claim, checked.verified,
                               dcr_reassembler=self.reassembler,
                               receipt_time=datetime.fromtimestamp(received_ns / 1e9, timezone.utc))
        except (L1sError, DcrDecodeError, DcxDecodeError) as exc:
            # 詳細はローカルログのみ。公開健全性には安定したコードを使う。
            _LOG.warning("semantic_decode_error sequence=%s type=%s: %s",
                         frame.sequence, type(exc).__name__, str(exc).replace("\n", " "))
            self.store.error("decode_errors", type(exc).__name__)
            return None
        if not alert.alert:
            return None
        result = self.events.observe(alert, now_us=time.monotonic_ns() // 1000)
        return self.store.publish(result, received_ns)


def receive(config: ReceiverConfig, store: AlertStore, stop: threading.Event,
            on_alert=None) -> None:
    pipeline = ReceiverPipeline(store, dedup_capacity=config.dedup_capacity,
                                dedup_ttl_seconds=config.dedup_ttl_seconds)
    store.start_input(replay=bool(config.replay))
    archive = None
    disk = None
    try:
        if config.raw_archive:
            archive = RawArchiveWriter(config.raw_archive, rotate_bytes=config.rotate_bytes,
                                       rotate_seconds=86400, fsync_interval=30)
            disk = DiskGuard(Path(config.raw_archive).parent, 256 * 1024**2,
                             128 * 1024**2, 64 * 1024**2)
        if config.replay:
            reader = RawArchiveReader(config.replay)
            frames = ((record.frame, record.pc_receive_ns) for record in reader)
            store.set_receiver("replay")
        else:
            def state(value):
                store.set_receiver(value, connected=value in ("waiting_hello", "connected"))
            receiver = SerialReceiver(config.port, config.baud,
                                      state_callback=state,
                                      status_callback=_LOG.info,
                                      error_callback=lambda count: store.error("bridge_errors", count=count))
            frames = receiver.frames(stop.is_set)
        try:
            for frame, received_ns in frames:
                if stop.is_set():
                    break
                if archive:
                    if not disk.check().allow_archive:
                        store.error("archive_errors")
                        raise OSError("archive disk reserve reached")
                    # 意味デコード前に検証済み Bridge レコードを保存する。
                    try:
                        archive.write(frame.wire, received_ns)
                    except OSError:
                        store.error("archive_errors")
                        raise
                record = pipeline.process(frame, received_ns)
                if record and on_alert:
                    on_alert(record)
        finally:
            frames.close()
        if config.replay:
            if reader.corrupt_records or reader.stats.skipped_bytes:
                store.error("archive_errors", count=max(1, reader.corrupt_records))
            store.set_receiver("stopped" if stop.is_set() else "replay_complete")
        else:
            store.set_receiver("stopped")
    except BaseException:
        store.set_receiver("failed")
        raise
    finally:
        if archive:
            archive.close()
