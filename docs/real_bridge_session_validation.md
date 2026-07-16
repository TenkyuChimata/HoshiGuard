# Real v0.2.5 Binary Bridge session validation

Evidence source is real STM32F103C8T6 + MAX-M10S hardware. Source SHA-256:
`dc8ab9a890278e64db1db0d1fa0bae1e56c03b8dc48066cdc7e9f03926885c89`.
Machine and Markdown reports are `reports/v025-session-report.json` and `.md`.

The 3,680.936-second archive contains 11,466 records: HELLO 62, L1S_FRAME
7,355, STATUS 368 and TIME_STATUS 3,681. Sequence 351..11816 is contiguous in
one epoch. Archive CRC, COBS, Bridge CRC, UBX checksum, L1S CRC and metadata
mismatch counts are zero.

The first STATUS (sequence 382) already had 238 pre-capture L1S. Between it
and the last STATUS (11813), the file has 7,333 L1S and 11,431 total frames.
Those equal L1S generated/sent and Bridge generated/sent deltas. Bridge, L1S
and Critical drop deltas are zero. Queue high watermark is 2; final PPS is
STABLE and time quality is `locked_utc_limited` (qErr unavailable, never
falsely confirmed). Peak wire rate is 478 B/s, 4.15% of 115200 8N1.

`test/fixtures/bridge_real_session/selected_records.hgraw` keeps exact wire
frames for HELLO, STATUS, TIME_STATUS, MT43/44/47/48/49/50, every observed PAB,
both svIds and end records without NAV-PVT/coordinates. Negative vectors are
explicitly `synthetic_derived_from_real` for archive, COBS, Bridge, sequence,
UBX, L1S and metadata rejection tests.

## 24-hour inherited-path certification

`captures/soak-24h.hgraw` contains 87,249.121 seconds from firmware 0.2.5,
build `Jul 15 2026 16:53:42`. Its 243,952 records comprise HELLO 1,455,
L1S_FRAME 146,520, STATUS 8,728 and TIME_STATUS 87,249. Sequence 2708..246659
is one epoch with no discontinuities. Archive/COBS/Bridge/UBX/L1S checks and
STM32/Python verification mismatches are all zero.

All 8,727 adjacent STATUS windows close after carrying the bounded in-flight
queue across snapshot boundaries; 240 windows have a strict sent snapshot
difference of one while generated/observed is exact. First-to-last counters,
all drop counters, and archive sequence close exactly. Queue high watermark is
4, final PPS is STABLE and time quality is `locked_utc_limited`.

This directly certifies firmware 0.2.5 and Bridge protocol/path 1.0. Because
the Bridge sources/wire behavior are unchanged, v0.2.6/v0.2.7 inherit protocol
and path evidence, but the current binary still requires a real-hardware
confirmation and cannot be labelled a 24-hour firmware soak. See
`docs/evidence_provenance.md`.
