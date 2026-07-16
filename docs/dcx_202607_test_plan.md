# July 2026 DCX test-broadcast validation plan

The supplied PDF describes six test scenarios, not binary message layout.
Scenario 1.1 covers A4 1–60 without the main ellipse; 1.2 covers 61–113 with
it. Both use FMMC and rotate all four severity codes. Scenario 2.1 uses FDMA,
2.2 Related Ministries, and exercises ten guidance patterns plus EX8/EX9
prefecture and municipality lists. Scenario 3.1/3.2 use Local Government with
A4 68/74/82, four severities, ten guidance values, EX1/EX2 and optional
additional ellipse EX3–EX7.

Run:

```bash
python3 tools/validate_dcx_test_scenario.py captures/dcx-202607.hgraw \
  --manifest test/fixtures/dcx_202607/scenario_manifest.json
```

The report separates received, decoded, decode_failed, CRC_failed, expected
coverage, unexpected values, duplicate count, provider/area coverage and
timing. A not-received test slot is not automatically a decoder failure:
actual emergency traffic may pre-empt scheduled test messages.

Schedule windows use section 1.2.2 table 1 in JST. The later overview figure
shows different 3.2 minute labels; that source discrepancy is recorded in the
manifest rather than silently normalized. Validator timing uses the PC receive
time stored in `.hgraw`, so host-clock error remains an external uncertainty.
