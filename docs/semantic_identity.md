# Semantic identity v2

HoshiGuard separates three identities:

1. Frame fingerprint hashes complete UBX evidence plus PAB/MT and intentionally
   changes when the transport wrapper changes.
2. Semantic fingerprint v2 hashes deterministic canonical JSON. It excludes
   Bridge sequence/timestamp, PC time, PPS, svId, PAB, checksums, raw UBX/L1S,
   word 9, flags, archive position/order/repeat count and localized labels. It
   retains service/spec/status/action, stable category, severity, normative raw
   time, provider/library, areas, ordered payload values, page identity and
   meaningful unknown/reserved codes.
3. Event key links lifecycle records with specification-explainable fields. It
   never uses translated names. When DCR/DCX lacks a global event identifier,
   confidence is `limited` and distinct semantics are not aggressively merged.

The v0.2.5 DCR hash accidentally included vendor `nmea`, `sentence`,
`preamble`, decode wall-clock `timestamp` and satellite placeholders. DCX
`decoded_fields` carried the same helpers. This made PAB/replay changes look
semantic. v2 uses DCR raw/code fields and DCX CAMF+ExtendedFields. DCR inferred
Gregorian year is presentation context: canonical identity keeps normative
month/day/hour/minute and normalizes inferred-year datetime values.

The event processor bounds active semantics/events, expires them by TTL and
caps stored frame fingerprints. Repeat summaries record count, first/last,
satellites and PABs. DCX Null and SLAS are service observations only. Raw
archive remains frame-complete.
