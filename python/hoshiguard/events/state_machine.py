from __future__ import annotations

from collections import OrderedDict
from dataclasses import dataclass, field

from ..models import UnifiedAlert
from .dedup import BoundedFingerprintSet


@dataclass(frozen=True)
class EventResult:
    disposition: str
    alert: UnifiedAlert
    repeat_count: int
    record_type: str = "semantic_observation"
    first_seen_us: int | None = None
    last_seen_us: int | None = None
    satellites_seen: tuple[int, ...] = ()
    pabs_seen: tuple[int, ...] = ()
    frame_fingerprints_seen: int = 0


@dataclass
class _Aggregate:
    alert: UnifiedAlert
    repeat_count: int
    first_seen_us: int
    last_seen_us: int
    satellites: set[int] = field(default_factory=set)
    pabs: set[int] = field(default_factory=set)
    frames: set[str] = field(default_factory=set)


@dataclass(frozen=True)
class RepeatSummary:
    service: str
    semantic_fingerprint: str
    semantic_fingerprint_version: int
    event_key: str
    repeat_count: int
    first_seen_us: int
    last_seen_us: int
    satellites_seen: tuple[int, ...]
    pabs_seen: tuple[int, ...]
    frame_fingerprints_seen: int
    observation: str
    alert: bool


class EventProcessor:
    """Bounded semantic/event state; raw archive remains frame-complete."""

    def __init__(self, capacity: int = 65_536,
                 ttl_us: int = 7 * 86_400_000_000,
                 maximum_frame_fingerprints: int = 64):
        self.capacity = capacity
        self.ttl_us = ttl_us
        self.maximum_frame_fingerprints = maximum_frame_fingerprints
        self.frames = BoundedFingerprintSet(capacity * 2)
        self.semantic: OrderedDict[str, _Aggregate] = OrderedDict()
        self.events: OrderedDict[tuple[str, str], tuple[str, int]] = OrderedDict()
        self.evictions = 0
        self.ttl_evictions = 0
        self.capacity_evictions = 0
        self.peak_semantic = 0
        self.peak_events = 0

    def _expire(self, now_us: int) -> None:
        if self.ttl_us <= 0:
            return
        # Both OrderedDicts are LRU/last-seen ordered because observe() pops
        # and reinserts touched entries. Expire only the oldest prefix; a full
        # scan per frame becomes quadratic during multi-day operation.
        while self.semantic:
            key = next(iter(self.semantic))
            item = self.semantic[key]
            if now_us < item.last_seen_us or now_us - item.last_seen_us <= self.ttl_us:
                break
            del self.semantic[key]
            self.evictions += 1
            self.ttl_evictions += 1
        while self.events:
            key = next(iter(self.events))
            _fingerprint, seen = self.events[key]
            if now_us < seen or now_us - seen <= self.ttl_us:
                break
            del self.events[key]

    @staticmethod
    def _result(disposition: str, alert: UnifiedAlert, item: _Aggregate,
                record_type: str = "semantic_observation") -> EventResult:
        return EventResult(disposition, alert, item.repeat_count, record_type,
                           item.first_seen_us, item.last_seen_us,
                           tuple(sorted(item.satellites)),
                           tuple(sorted(item.pabs)), len(item.frames))

    def observe(self, alert: UnifiedAlert) -> EventResult:
        now_us = alert.receipt.timestamp_us
        self._expire(now_us)
        self.frames.observe(alert.identity.frame_fingerprint)
        fingerprint = alert.identity.semantic_fingerprint
        item = self.semantic.pop(fingerprint, None)
        if item is None:
            item = _Aggregate(alert, 0, now_us, now_us)
        item.repeat_count += 1
        item.last_seen_us = now_us
        item.satellites.add(alert.receipt.sv_id)
        item.pabs.add(alert.receipt.pab)
        if len(item.frames) < self.maximum_frame_fingerprints:
            item.frames.add(alert.identity.frame_fingerprint)
        self.semantic[fingerprint] = item
        while len(self.semantic) > self.capacity:
            self.semantic.popitem(last=False)
            self.evictions += 1
            self.capacity_evictions += 1
        self.peak_semantic = max(self.peak_semantic, len(self.semantic))
        if item.repeat_count > 1:
            return self._result("repeated", alert, item)
        # SLAS monitoring, NULL, reserved and test-mode transport frames are
        # observations, not disaster-event lifecycle transitions. They still
        # participate in dedup and raw archival, but must never be labelled
        # "issued" merely because this is their first occurrence.
        if not alert.alert:
            return self._result("observed", alert, item)
        key = (alert.status, alert.identity.event_key)
        previous_entry = self.events.pop(key, None)
        previous = previous_entry[0] if previous_entry else None
        if alert.action == "cancel":
            disposition = "cancelled"
        elif alert.action == "clear":
            disposition = "cleared"
        elif alert.action == "update":
            disposition = "updated"
        elif previous is None:
            disposition = "issued"
        elif previous != alert.identity.semantic_fingerprint:
            disposition = "updated"
        else:
            disposition = "repeated"
        self.events[key] = (alert.identity.semantic_fingerprint, now_us)
        if len(self.events) > self.capacity:
            self.events.popitem(last=False)
            self.evictions += 1
            self.capacity_evictions += 1
        self.peak_events = max(self.peak_events, len(self.events))
        return self._result(disposition, alert, item, "event_lifecycle")

    def repeat_summaries(self) -> tuple[RepeatSummary, ...]:
        result = []
        for fingerprint, item in self.semantic.items():
            if item.repeat_count <= 1:
                continue
            token = item.alert.category.token or f"code_{item.alert.category.code}"
            result.append(RepeatSummary(
                item.alert.service.value, fingerprint,
                item.alert.identity.semantic_fingerprint_version,
                item.alert.identity.event_key, item.repeat_count,
                item.first_seen_us, item.last_seen_us,
                tuple(sorted(item.satellites)), tuple(sorted(item.pabs)),
                len(item.frames), token, item.alert.alert))
        return tuple(result)
