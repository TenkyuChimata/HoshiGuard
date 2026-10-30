"""Bounded, atomic public state shared by the receiver and HTTP/WS threads.

The receiver never awaits clients. Queues contain references to pre-encoded
commits; one coalesced event-loop callback wakes all subscribers. Overflow
forces resynchronisation instead of silently losing an intermediate cancel.
"""
from __future__ import annotations

import asyncio
from collections import OrderedDict, deque
from dataclasses import dataclass, field
import json
import threading
import time
import uuid

from .events.state_machine import EventResult
from .models import json_safe
from .public import PublicAlert, utc_iso


def encode(value) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False)


@dataclass(frozen=True)
class PublishedAlert:
    id: str
    service: str
    category: str
    status: str
    sequence: int
    encoded: str
    encoded_bytes: int
    committed: float
    state_key: str


@dataclass(eq=False)
class Subscription:
    wake: asyncio.Event = field(default_factory=asyncio.Event)
    messages: deque[str] = field(default_factory=deque)
    overflow: bool = False
    status_filter: str = "actual"


class AlertStore:
    def __init__(self, *, capacity: int = 512, ttl_seconds: float = 86400,
                 queue_capacity: int = 32, maximum_clients: int = 64,
                 replay: bool = False, clock=time.monotonic):
        if not 1 <= capacity <= 4096 or ttl_seconds <= 0 or not 1 <= queue_capacity <= 256 or not 1 <= maximum_clients <= 256:
            raise ValueError("invalid publisher limits")
        self.capacity, self.ttl_seconds = capacity, ttl_seconds
        self.queue_capacity, self.maximum_clients = queue_capacity, maximum_clients
        self.replay, self.clock = replay, clock
        self.process_id = str(uuid.uuid4())
        self.sequence = 0
        self._lock = threading.Lock()
        self._history: OrderedDict[str, PublishedAlert] = OrderedDict()
        self._states: OrderedDict[str, PublishedAlert] = OrderedDict()
        self._subscribers: set[Subscription] = set()
        self._loop: asyncio.AbstractEventLoop | None = None
        self._wake_pending = False
        self._started = clock()
        self._last_frame = self._last_l1s = None
        self._status = {"receiver": "replay" if replay else "starting",
                        "connected": False, "last_frame_received": None,
                        "last_l1s_received": None, "last_alert_received": None,
                        "verification_errors": 0, "decode_errors": 0,
                        "last_decode_error": None, "sequence_gaps": 0,
                        "archive_errors": 0, "bridge_errors": 0,
                        "upstream_drops": 0, "upstream_l1s_drops": 0, "upstream_errors": 0,
                        "slow_clients_disconnected": 0}
        self._health_key = None

    def start_input(self, *, replay: bool) -> None:
        with self._lock:
            if self._history:
                raise RuntimeError("an input is already active")
            self.replay = replay

    def attach(self, loop: asyncio.AbstractEventLoop) -> None:
        with self._lock:
            if self._loop is not None:
                raise RuntimeError("publisher already attached")
            self._loop = loop

    def detach(self) -> None:
        with self._lock:
            self._loop = None
            self._wake_pending = False
            self._subscribers.clear()

    def _wake(self) -> None:
        with self._lock:
            self._wake_pending = False
            for sub in self._subscribers:
                sub.wake.set()

    def _broadcast_locked(self, kind: str, data: str, alert_status: str | None = None) -> None:
        self.sequence += 1
        message = ('{"type":' + encode(kind) + ',"process_id":' + encode(self.process_id)
                   + ',"sequence":' + str(self.sequence) + ',"data":' + data + '}')
        for sub in self._subscribers:
            if alert_status and sub.status_filter not in ("all", alert_status):
                continue
            if sub.overflow:
                continue
            if len(sub.messages) >= self.queue_capacity:
                sub.messages.clear()
                sub.overflow = True
                self._status["slow_clients_disconnected"] += 1
            else:
                sub.messages.append(message)
        if self._subscribers and self._loop is not None and not self._wake_pending:
            self._wake_pending = True
            try:
                self._loop.call_soon_threadsafe(self._wake)
            except RuntimeError:  # shutdown raced with a final receiver status
                self._wake_pending = False

    def _expire_locked(self) -> None:
        now = self.clock()
        while self._history:
            key, record = next(iter(self._history.items()))
            if now - record.committed <= self.ttl_seconds:
                break
            del self._history[key]
        while self._states:
            key, record = next(iter(self._states.items()))
            if now - record.committed <= self.ttl_seconds:
                break
            del self._states[key]

    def publish(self, result: EventResult, received_ns: int) -> PublishedAlert | None:
        alert = result.alert
        if result.disposition == "repeated" or not alert.alert or alert.service.value not in ("DCR", "DCX"):
            return None
        if not all((alert.validity.bridge_crc, alert.validity.ubx,
                    alert.validity.l1s_crc, alert.validity.structure, alert.validity.semantics)):
            return None
        public = PublicAlert.from_alert(alert, received_ns, replay=self.replay)
        encoded = encode(json_safe(public))
        if len(encoded.encode("utf-8")) > 65536:
            self.error("decode_errors", "public_size_limit")
            return None
        # No reliable event ID is carried in most reports. Keep each message
        # state independently, retaining explicit action and correlation hint.
        state_key = (f"{alert.service.value}:{public.status}:{alert.identity.event_key}"
                     if alert.identity.confidence == "spec_scoped" else public.id)
        with self._lock:
            self._expire_locked()
            record = PublishedAlert(public.id, public.service,
                                    public.category.get("token") or "unknown", public.status,
                                    self.sequence + 1, encoded, len(encoded.encode("utf-8")), self.clock(), state_key)
            self._history.pop(record.id, None)
            self._history[record.id] = record
            self._states.pop(state_key, None)
            self._states[state_key] = record
            for cache in (self._history, self._states):
                while len(cache) > self.capacity:
                    cache.popitem(last=False)
            self._status["last_alert_received"] = utc_iso(received_ns)
            self._broadcast_locked("alert", encoded, public.status)
            return record

    def set_receiver(self, state: str, *, connected: bool = False) -> None:
        with self._lock:
            self._status.update(receiver=state, connected=connected)
            self._notify_health_locked()

    def frame_received(self, received_ns: int, *, l1s: bool = False) -> None:
        with self._lock:
            self._last_frame = self.clock()
            self._status["last_frame_received"] = utc_iso(received_ns)
            if l1s:
                self._last_l1s = self.clock()
                self._status["last_l1s_received"] = utc_iso(received_ns)
            self._notify_health_locked()

    def error(self, counter: str, code: str | None = None, count: int = 1) -> None:
        if counter not in ("verification_errors", "decode_errors", "sequence_gaps", "archive_errors", "bridge_errors", "upstream_drops", "upstream_l1s_drops", "upstream_errors"):
            raise ValueError("unknown health counter")
        with self._lock:
            self._status[counter] += count
            if counter == "decode_errors":
                self._status["last_decode_error"] = code
            self._notify_health_locked()

    def _status_locked(self) -> dict:
        now = self.clock()
        state = self._status["receiver"]
        connected = self._status["connected"]
        bridge_age = now - (self._last_frame if self._last_frame is not None else self._started)
        signal_age = now - (self._last_l1s if self._last_l1s is not None else self._started)
        if self.replay:
            link, signal = "historical", "historical"
        elif not connected:
            link, signal = state, "unknown"
        elif bridge_age >= 15:
            link, signal = "stalled", "unknown"
        elif state == "waiting_hello":
            link, signal = state, "unknown"
        else:
            link = "connected"
            signal = "no_l1s" if signal_age >= 30 else "receiving" if self._last_l1s is not None else "waiting_l1s"
        return {"schema": "hoshiguard.status.v1", "process_id": self.process_id,
                "sequence": self.sequence, **self._status,
                "link": link, "signal": signal,
                "data_mode": "live" if link == "connected" and signal == "receiving" else "historical",
                "replay": self.replay, "alerts_cached": len(self._history),
                "states_cached": len(self._states), "clients": len(self._subscribers),
                "capacity": self.capacity, "ttl_seconds": self.ttl_seconds,
                "uptime_seconds": round(now - self._started, 3)}

    def _notify_health_locked(self) -> None:
        value = self._status_locked()
        key = tuple(value[name] for name in ("receiver", "link", "signal", "decode_errors", "verification_errors", "archive_errors", "sequence_gaps", "bridge_errors", "upstream_drops", "upstream_l1s_drops", "upstream_errors"))
        if key != self._health_key:
            self._health_key = key
            value["sequence"] = self.sequence + 1
            self._broadcast_locked("status", encode(value))

    def refresh_health(self) -> None:
        # Only health expiry uses a timer. Business publication is event driven.
        with self._lock:
            self._expire_locked()
            self._notify_health_locked()

    def status(self) -> dict:
        with self._lock:
            self._expire_locked()
            return self._status_locked()

    def query(self, *, identifier: str | None = None, service: str | None = None,
              category: str | None = None, status: str | None = None,
              limit: int = 100, states: bool = False, offset: int = 0) -> tuple[list[PublishedAlert], dict]:
        with self._lock:
            self._expire_locked()
            cache = self._states if states else self._history
            records = []
            size = 0
            skipped = 0
            for record in reversed(cache.values()):
                if identifier and record.id != identifier:
                    continue
                if service and record.service != service or category and record.category != category or status and record.status != status:
                    continue
                if skipped < offset:
                    skipped += 1
                    continue
                size += record.encoded_bytes
                if size > 1024 * 1024 or len(records) >= limit:
                    break
                records.append(record)
            return records, self._status_locked()

    def subscribe(self, status: str = "actual") -> tuple[Subscription, str]:
        with self._lock:
            if len(self._subscribers) >= self.maximum_clients:
                raise OverflowError("client limit")
            self._expire_locked()
            sub = Subscription(status_filter=status)
            # Snapshot and registration share the same commit lock. Nothing can
            # be lost or delivered twice at the snapshot/live boundary.
            available = [r for r in reversed(self._states.values())
                         if status == "all" or r.status == status]
            records = available[:100]
            size = sum(r.encoded_bytes for r in records)
            while size > 1024 * 1024:
                size -= records.pop().encoded_bytes
            snapshot = ('{"type":"snapshot","process_id":' + encode(self.process_id)
                        + ',"sequence":' + str(self.sequence)
                        + ',"data":{"alerts":[' + ','.join(r.encoded for r in records)
                        + '],"truncated":' + encode(len(records) < len(available))
                        + ',"status":' + encode(self._status_locked()) + '}}')
            self._subscribers.add(sub)
            return sub, snapshot

    def unsubscribe(self, sub: Subscription) -> None:
        with self._lock:
            self._subscribers.discard(sub)
            sub.messages.clear()

    def pop(self, sub: Subscription) -> tuple[bool, str | None]:
        with self._lock:
            if sub.messages:
                return sub.overflow, sub.messages.popleft()
            sub.wake.clear()
            return sub.overflow, None
