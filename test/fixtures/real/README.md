# Field fixture provenance

- capture date: 2026-07-14
- receiver: u-blox MAX-M10S, SPG 5.10
- region: withheld
- coordinates: not retained
- semantic status: transport/PAB/MT/CRC verified; DCR/DCX fields not manually
  interpreted by this fixture set

The generated `manifest.json` is authoritative. Headers contain only complete
RXM-SFRBX packets needed by the regression tests. Run
`tools/import_real_fixtures.py capture-outdoor.log --check` to verify that the
headers, manifest, capture hash and deterministic selection still match.
