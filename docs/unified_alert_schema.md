# Unified alert schema

Human and NDJSON output are generated from the same frozen `UnifiedAlert`
model. The stable schema identifier is `hoshiguard.alert.v1`. It contains
service/spec/receipt/validity/status/action/identity/category/severity/times,
target areas, a typed DCR or DCX payload, unknown fields and optional complete
raw evidence. Target areas are warning targets, never receiver coordinates.

`frame_fingerprint` identifies transport evidence and may differ by PAB,
satellite or UBX wrapper. `semantic_fingerprint_version=2` hashes the canonical
semantic representation and excludes receipt/Bridge/PPS, svId, PAB, UBX/L1S
raw, checksums, flags and presentation labels. Status/action/category,
normative raw time, provider/library, areas, ordered payload values, page
identity and meaningful unknown/reserved codes remain.

`event_key` links lifecycle messages in separate test/actual spaces using only
specification-explainable fields. If no globally unique event identifier is
available, confidence is `limited` and merging stays conservative. NDJSON
`record_type` is one of `frame_observation`, `semantic_observation`,
`event_lifecycle`, or `service_summary`. Repeats have bounded aggregate
summaries; raw archive retains every Bridge frame regardless of output mode.

v0.2.7 adds compatible top-level provenance fields to every business record:
`stream_mode`, `producer_version`, `semantic_fingerprint_version`,
`bridge_protocol_version`, and `session_id`. A file normally also contains
`hoshiguard.session.v1` `session_start` and `session_end` records. Consumers of
`hoshiguard.alert.v1` should ignore unfamiliar record types; deployments that
require the old one-alert-per-line-only form can use `hoshiguard diagnose --no-session-records` with the relevant input.

The production HTTP/WS projection is `hoshiguard.public-alert.v1`; see [API](api.md).
Since 0.3.0, limited event keys no longer infer updates or merge independent reports.
Detailed observation/session output is only enabled under `hoshiguard diagnose`.
