from __future__ import annotations

import json
from datetime import datetime, timezone
from hashlib import sha256
import os
from pathlib import Path
import sys
import time
from typing import TextIO
import uuid

from ..events.state_machine import EventResult, RepeatSummary
from ..models import json_safe


class NdjsonWriter:
    def __init__(self, destination: str, include_raw: bool = True,
                 flush_every: int = 1, *, stream_mode: str | None = None,
                 producer_version: str = "0.2.7",
                 bridge_protocol_version: str = "1.0",
                 session_id: str | None = None, source: str | None = None,
                 session_records: bool = False, rotate_bytes: int = 0,
                 rotate_seconds: float = 0, fsync_interval: float = 0):
        self.destination = destination
        self.include_raw = include_raw
        self.flush_every = max(1, flush_every)
        self.stream_mode = stream_mode
        self.producer_version = producer_version
        self.bridge_protocol_version = bridge_protocol_version
        self.session_id = session_id or str(uuid.uuid4())
        self.source = source
        self.session_records = session_records
        self.rotate_bytes = max(0, rotate_bytes)
        self.rotate_seconds = max(0.0, rotate_seconds)
        self.fsync_interval = max(0.0, fsync_interval)
        self.count = 0
        self.business_count = 0
        self.segment_business_count = 0
        self.segment = 0
        self.started_monotonic = time.monotonic()
        self.last_fsync = self.started_monotonic
        self.started_at = self._now()
        self._owned = destination != "-"
        self.base = Path(destination) if self._owned else None
        self.path = self.base
        self.stream: TextIO = sys.stdout
        self._digest = sha256()
        self._open()

    @staticmethod
    def _now() -> str:
        return datetime.now(timezone.utc).isoformat()

    def _segment_path(self) -> Path:
        assert self.base is not None
        if self.segment == 0:
            return self.base
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
        return self.base.with_name(
            f"{self.base.stem}.{stamp}.{self.segment:04d}{self.base.suffix}")

    def _open(self) -> None:
        self._digest = sha256()
        if self._owned:
            self.path = self._segment_path()
            assert self.path is not None
            self.path.parent.mkdir(parents=True, exist_ok=True)
            self.stream = self.path.open("x", encoding="utf-8")
        self.segment_business_count = 0
        self.started_monotonic = time.monotonic()
        self.last_fsync = self.started_monotonic
        if self.session_records:
            self._write_value({
                "record_type": "session_start",
                "schema": "hoshiguard.session.v1",
                "stream_mode": self.stream_mode,
                "producer_version": self.producer_version,
                "semantic_fingerprint_version": 2,
                "bridge_protocol_version": self.bridge_protocol_version,
                "started_at": self._now(),
                "source": self.source,
                "session_id": self.session_id,
                "segment": self.segment,
            }, business=False, allow_rotate=False)

    def _sha_sidecar(self) -> None:
        if not self._owned or self.path is None:
            return
        self.path.with_suffix(self.path.suffix + ".sha256").write_text(
            f"{self._digest.hexdigest()}  {self.path.name}\n", encoding="ascii")

    def _finish_segment(self, clean: bool) -> None:
        if self.session_records and clean:
            self._write_value({
                "record_type": "session_end",
                "schema": "hoshiguard.session.v1",
                "stream_mode": self.stream_mode,
                "producer_version": self.producer_version,
                "session_id": self.session_id,
                "segment": self.segment,
                "objects_written": self.segment_business_count,
                "ended_at": self._now(),
                "clean_shutdown": clean,
            }, business=False, allow_rotate=False)
        self.stream.flush()
        if self._owned:
            os.fsync(self.stream.fileno())
            self.stream.close()
            self._sha_sidecar()

    def _needs_rotate(self, encoded_bytes: int) -> bool:
        if not self._owned:
            return False
        elapsed = time.monotonic() - self.started_monotonic
        by_time = self.rotate_seconds and elapsed >= self.rotate_seconds
        by_size = False
        if self.rotate_bytes:
            by_size = self.stream.tell() + encoded_bytes > self.rotate_bytes
        return bool(by_time or by_size)

    def _rotate(self) -> None:
        self._finish_segment(clean=True)
        self.segment += 1
        self._open()

    def _write_value(self, value: dict, *, business: bool,
                     allow_rotate: bool = True) -> None:
        if business:
            value = dict(value)
            value["stream_mode"] = self.stream_mode
            value["producer_version"] = self.producer_version
            value["semantic_fingerprint_version"] = value.get(
                "identity", {}).get("semantic_fingerprint_version", 2)
            value["bridge_protocol_version"] = self.bridge_protocol_version
            value["session_id"] = self.session_id
        line = json.dumps(value, ensure_ascii=False, separators=(",", ":"),
                          allow_nan=False) + "\n"
        encoded = line.encode("utf-8")
        if allow_rotate and self._needs_rotate(len(encoded)):
            self._rotate()
        self.stream.write(line)
        if self._owned:
            self._digest.update(encoded)
        self.count += 1
        if business:
            self.business_count += 1
            self.segment_business_count += 1
        now = time.monotonic()
        if self.count % self.flush_every == 0:
            self.stream.flush()
        if (self._owned and self.fsync_interval and
                now - self.last_fsync >= self.fsync_interval):
            self.stream.flush()
            os.fsync(self.stream.fileno())
            self.last_fsync = now

    def write(self, result: EventResult) -> None:
        value = json_safe(result.alert)
        value["record_type"] = result.record_type
        value["event"] = {"disposition": result.disposition,
                          "repeat_count": result.repeat_count,
                          "first_seen_us": result.first_seen_us,
                          "last_seen_us": result.last_seen_us,
                          "satellites_seen": list(result.satellites_seen),
                          "pabs_seen": list(result.pabs_seen),
                          "frame_fingerprints_seen": result.frame_fingerprints_seen}
        if not self.include_raw:
            value.pop("raw", None)
        self._write_value(value, business=True)

    def write_repeat_summary(self, summary: RepeatSummary) -> None:
        value = {"schema": "hoshiguard.service_summary.v1",
                 "record_type": "service_summary", **json_safe(summary)}
        self._write_value(value, business=True)

    def close(self, clean: bool = True) -> None:
        if self.stream.closed:
            return
        self._finish_segment(clean)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, _exc, _tb):
        self.close(clean=exc_type is None)
