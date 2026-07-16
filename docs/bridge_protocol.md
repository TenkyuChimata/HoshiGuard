# HoshiGuard Bridge Protocol v1.0

The wire format is `COBS(logical frame) + 00`. Multi-byte integers are little
endian. CRC is CRC-32/ISO-HDLC: polynomial `0x04C11DB7` (reflected
implementation `0xEDB88320`), init `FFFFFFFF`, refin/refout true, xorout
`FFFFFFFF`, check `CBF43926` for `123456789`. CRC is serialized little endian.

## Fixed header (24 bytes)

| Offset | Width | Field |
|---:|---:|---|
| 0 | 2 | `HG` |
| 2 | 1 | major (1) |
| 3 | 1 | minor (0) |
| 4 | 1 | frame type |
| 5 | 1 | reserved |
| 6 | 2 | header length (24) |
| 8 | 2 | payload length |
| 10 | 2 | header flags |
| 12 | 4 | sequence |
| 16 | 8 | extended STM32 timestamp_us |

Payload and little-endian CRC32 follow. Major mismatch is rejected; a newer
minor is accepted when its fixed header/payload length remains understood.

Frame types are HELLO=1, L1S_FRAME=2, STATUS=3, TIME_STATUS=4, ERROR=5 and
HEARTBEAT=6. HELLO repeats every 60 seconds. A HELLO whose sequence returns to
a small value establishes a new restart epoch; it is not reported as an
ordinary gap. Sequence arithmetic is modulo 2^32.

L1S_FRAME payload starts with 20 bytes: PPS sequence (U4), svId, gnssId, sigId,
numWords, PAB, MT, flags (U2), full UBX length (U2), time quality, PPS state,
firmware major/minor/patch and reserved. The complete UBX-RXM-SFRBX packet
follows, including sync, length, all 9 words and UBX checksum. Python repeats
every validation and rejects flag mismatches from semantic routing.

The fixed seven-slot binary queue reserves four slots for L1S. An active COBS
record cannot be evicted, so partial USART writes remain atomic. Critical and
L1S may evict STATUS/TIME; low-priority traffic cannot consume reserved slots.
No enqueue operation waits for the UART.

TIME_STATUS is 32 bytes and preserves, in order, PPS sequence, PPS/quality/
reason enums, TIM-TP flags, signed qErr in official picoseconds plus its
validity flag, refInfo, GPS week, raw towMS/towSubMS, matched PPS sequence and
matched/ambiguous/time-grid-known bits. An unavailable qErr remains `null` in
Python; its numeric placeholder is never interpreted as zero error.

## v0.2.7 compatibility and STATUS closure

v0.2.7 does not change protocol major/minor or any wire field. Semantic
identity and stream filtering are PC-side concerns. STATUS v1.0 already has
Bridge generated/queued/sent/dropped, Critical dropped and separate L1S
generated/sent/dropped counters. Session validation compares deltas between
the first and last STATUS with frames strictly inside that sequence window, so
pre-capture cumulative counts are not mistaken for archive records. HELLO
sequence reset starts a new epoch; normal rollover remains modulo 2^32.

For adjacent STATUS snapshots, `generated` closes directly against observed
sequence records. A STATUS payload is encoded before it has necessarily drained
from the queue, so `sent` can differ by one at an individual boundary. The soak
validator carries `generated-sent` pending depth across the boundary and reports
both strict snapshot and queue-adjusted closure; it never hides the distinction.
