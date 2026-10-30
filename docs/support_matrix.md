# Alert support matrix and verification levels

Machine-readable truth is [`support_matrix.json`](support_matrix.json). v0.2.7
separates implementation from evidence:

- `unsupported`, `raw_only`, `implemented`
- `synthetic_verified`
- `official_scenario_defined` / `official_scenario_verified`
- `real_broadcast_verified`

Decoder presence does not imply real-broadcast verification. DCR-017 retains DCR-016 wire layouts and has
implementations for active categories 1,2,3,4,5,6,8,9,10,11,12,14. The July
2026 official scenario defines categories 1,2,3,4,5,8,9,10,11, but no matching
official broadcast capture is yet present, so those entries are not promoted
to `official_scenario_verified`. The v0.2.5 real session promotes only content
actually observed, including DCR Marine (14) and DCX Null.

DCX CAMF A1–A18, A4 1–113, EX1–EX9 and reserved EX10 are implemented and
synthetically verified. EX11 remains `raw_only`, `wrapper_known=true`, and
`blocked_by=missing_country_specific_spec`; no country-specific 68-bit meaning
is guessed. Municipality identity uses raw code. The bundled names remain
`name_source=snapshot, authoritative=false` until a versioned authoritative
JIS X 0402 source is independently audited.

DCX-004 places CAMF and one Extended Message in one MT44 and specifies no
cross-frame reassembly. DCR Nankai Pn/Pm reassembly is bounded and timed.
“Not verified” is not “not implemented,” and a preempted official test message
is `not_received`, not a decoder failure.

The Bridge 1.0 transport/path now has 87,249.121 seconds of real-hardware
evidence from firmware 0.2.5: zero archive/COBS/Bridge/UBX/L1S errors, zero
sequence gaps, and zero Bridge/L1S/Critical drops. This is inherited path
evidence for unchanged v0.2.6/v0.2.7 code, not a 24-hour certification of those
firmware binaries. The soak actually observed DCR14 Marine, DCX Null and 69
non-Null `l_alert_ex1` DCX frames; no unobserved category is promoted.

The 017 additive time/code changes are synthetically verified only; see
[dcr017_audit.md](dcr017_audit.md). No new real-broadcast evidence level is
assigned by the HTTP/WS refactor. EX9 prefecture targets now exclude its
17 reserved bits; Japan non-Null Vn other than 1 is not fully supported.
