"""原子的な状態更新と明示的なディスク方針を備えた運用補助。"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import time
from typing import Any


def parse_quantity(value: str) -> int:
    match = re.fullmatch(r"\s*(\d+(?:\.\d+)?)\s*([KMGTP]?)(?:i?B)?\s*",
                         value, re.IGNORECASE)
    if not match:
        raise ValueError(f"invalid byte quantity: {value}")
    scale = {"": 1, "K": 1024, "M": 1024 ** 2, "G": 1024 ** 3,
             "T": 1024 ** 4, "P": 1024 ** 5}[match.group(2).upper()]
    return int(float(match.group(1)) * scale)


def parse_duration(value: str) -> float:
    match = re.fullmatch(r"\s*(\d+(?:\.\d+)?)\s*([smhd]?)\s*",
                         value, re.IGNORECASE)
    if not match:
        raise ValueError(f"invalid duration: {value}")
    scale = {"": 1, "s": 1, "m": 60, "h": 3600, "d": 86400}
    return float(match.group(1)) * scale[match.group(2).lower()]


def atomic_json(path: str | Path, value: dict[str, Any]) -> None:
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(f".{target.name}.{os.getpid()}.tmp")
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, target)


@dataclass(frozen=True)
class DiskDecision:
    level: str
    free_bytes: int
    allow_archive: bool
    allow_ndjson: bool


class DiskGuard:
    """派生出力より raw 証跡を優先し、黙ってファイルを破損させない。"""

    def __init__(self, directory: str | Path, warning_bytes: int,
                 critical_bytes: int, stop_bytes: int,
                 critical_action: str = "drop-ndjson",
                 check_interval_seconds: float = 1.0):
        if not 0 <= stop_bytes <= critical_bytes <= warning_bytes:
            raise ValueError("disk thresholds must satisfy stop <= critical <= warning")
        self.directory = Path(directory)
        self.warning = warning_bytes
        self.critical = critical_bytes
        self.stop = stop_bytes
        self.critical_action = critical_action
        self.check_interval = max(0.0, check_interval_seconds)
        self.last_check = 0.0
        self.last_decision: DiskDecision | None = None

    def check(self) -> DiskDecision:
        now = time.monotonic()
        if (self.last_decision is not None and
                now - self.last_check < self.check_interval):
            return self.last_decision
        self.directory.mkdir(parents=True, exist_ok=True)
        stat = os.statvfs(self.directory)
        free = stat.f_bavail * stat.f_frsize
        if free <= self.stop:
            decision = DiskDecision("stop", free, False, False)
            self.last_check, self.last_decision = now, decision
            return decision
        if free <= self.critical:
            if self.critical_action == "stop":
                decision = DiskDecision("critical", free, False, False)
            elif self.critical_action == "drop-archive":
                decision = DiskDecision("critical", free, False, True)
            else:
                decision = DiskDecision("critical", free, True, False)
            self.last_check, self.last_decision = now, decision
            return decision
        if free <= self.warning:
            decision = DiskDecision("warning", free, True, True)
        else:
            decision = DiskDecision("ok", free, True, True)
        self.last_check, self.last_decision = now, decision
        return decision


class HealthReporter:
    def __init__(self, path: str, initial: dict[str, Any] | None = None,
                 interval_seconds: float = 1.0):
        self.path = Path(path)
        self.started = time.monotonic()
        self.interval = max(0.0, interval_seconds)
        self.last_write = 0.0
        self.value: dict[str, Any] = dict(initial or {})

    def update(self, *, force: bool = False, **values: Any) -> None:
        self.value.update(values)
        now = time.monotonic()
        if not force and now - self.last_write < self.interval:
            return
        self.value["current_time"] = datetime.now(timezone.utc).isoformat()
        self.value["process_uptime_seconds"] = now - self.started
        atomic_json(self.path, self.value)
        self.last_write = now


class PidFile:
    def __init__(self, path: str | None):
        self.path = Path(path) if path else None

    def __enter__(self):
        if self.path:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            fd = os.open(self.path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
            with os.fdopen(fd, "w", encoding="ascii") as stream:
                stream.write(f"{os.getpid()}\n")
        return self

    def __exit__(self, *_args):
        if self.path:
            try:
                self.path.unlink()
            except FileNotFoundError:
                pass
