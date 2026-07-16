"""Bounded DCR-016 multi-page reassembly for Nankai Trough text."""

from __future__ import annotations

from dataclasses import dataclass
import time


@dataclass
class _Assembly:
    total: int
    pages: dict[int, bytes]
    updated: float


class NankaiTroughReassembler:
    def __init__(self, capacity: int = 8, timeout_seconds: float = 180.0):
        self.capacity = capacity
        self.timeout_seconds = timeout_seconds
        self._items: dict[tuple, _Assembly] = {}

    def add(self, key: tuple, page: int, total: int, text: bytes,
            now: float | None = None) -> bytes | None:
        now = time.monotonic() if now is None else now
        self.expire(now)
        if not (1 <= page <= total <= 63):
            return None
        if key not in self._items and len(self._items) >= self.capacity:
            oldest = min(self._items, key=lambda item: self._items[item].updated)
            del self._items[oldest]
        item = self._items.setdefault(key, _Assembly(total, {}, now))
        if item.total != total:
            item.total, item.pages = total, {}
        item.pages[page] = text
        item.updated = now
        if len(item.pages) != total:
            return None
        result = b"".join(item.pages[index] for index in range(1, total + 1))
        del self._items[key]
        return result

    def expire(self, now: float | None = None) -> None:
        now = time.monotonic() if now is None else now
        for key in [key for key, value in self._items.items()
                    if now - value.updated > self.timeout_seconds]:
            del self._items[key]

