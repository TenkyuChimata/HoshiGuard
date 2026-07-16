# Real Bridge session regression fixture

Derived deterministically from SHA-256 `dc8ab9a890278e64db1db0d1fa0bae1e56c03b8dc48066cdc7e9f03926885c89`.  The original hour-long
archive is not copied here. `selected_records.hgraw` retains exact Bridge wire
frames and PC receive timestamps for HELLO, STATUS, TIME_STATUS, MT43/44/47-50,
all observed PABs, both observed satellites, and records near both ends.

Evidence source: `real_hardware`. Coordinates and NAV-PVT are not retained.
`negative_vectors.json` is explicitly synthetic-derived and must never be
reported as a real receiver failure.
