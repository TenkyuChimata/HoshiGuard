# Specification inventory

This inventory was audited on 2026-07-15. Hashes and machine-readable status
are in `inventory.json`. Normative PDFs are not vendored; their official URLs
and audited SHA-256 values make the audit repeatable.

| Tier | Document | Version/date | Parser use | Status |
|---|---|---|---|---|
| 1 | IS-QZSS-L1S | 009, 2026-02-20 | L1S transport/CRC/routing | complete |
| 1 | IS-QZSS-DCR | 016, 2026-04-03 | MT43 | complete |
| 1 | IS-QZSS-DCX | 004, 2026-05-29 | MT44 envelope/extensions | complete |
| 1 | EWSS CAMF | Issue 1.2, 2026-03 | A1–A18/A4 library | complete |
| 1 | JIS X 0402 current dataset | not supplied/audited | municipality names | **blocked** |
| 1 | EX11 country libraries | country-dependent | outside-Japan extension | **blocked** |
| 2 | users-manual_ver2.pdf | app note v2, based on DCR-013 | context only | complete auxiliary copy |
| 2 | dcx_test_data_details_202607.pdf | July 2026 | scenario coverage only | complete auxiliary copy |
| 2 | details_202607.pdf | July 2026 | DCR scenario oracle only | complete auxiliary copy |

The DCR-016 PDF itself says April 03, 2026. That internal date is used here.
The older application note never overrides DCR-016 offsets or enumerations.
The July test plan never defines DCX bit layout and is never used to fabricate
broadcast binary fixtures.
