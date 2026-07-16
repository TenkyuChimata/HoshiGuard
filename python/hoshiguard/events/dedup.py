from collections import OrderedDict


class BoundedFingerprintSet:
    def __init__(self, capacity: int = 256):
        self.capacity = capacity
        self._items: OrderedDict[str, int] = OrderedDict()

    def observe(self, fingerprint: str) -> int:
        count = self._items.pop(fingerprint, 0) + 1
        self._items[fingerprint] = count
        while len(self._items) > self.capacity:
            self._items.popitem(last=False)
        return count

