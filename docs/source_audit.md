# Source audit

Normative semantics come from L1S-009, DCR-016, DCX-004 and CAMF 1.2 listed in
the specification inventory. `users-manual_ver2.pdf` is useful for delivery
cycles, priority and presentation examples, but it is based on DCR-013.
`dcx_test_data_details_202607.pdf` describes a test broadcast schedule and
coverage; it does not define bit offsets.

`details_202607.pdf` (SHA-256
`29b515eca53906e7e3c7002bdf6a942c30673621adc8fccca359d6375b144caa`)
is the official July 2026 DCR training/test scenario oracle. It defines dates,
Rc=7, scenario values, repetition/sorting and page/part composition. It is not
a bit-layout specification and no binary fixture is fabricated from it;
DCR-016 remains normative for decoding.

The exhaustive Python semantic core is the MIT-licensed `azarashi` 0.16.3
source vendored under `python/hoshiguard/_vendor/azarashi/`, with its license.
The audited wheel SHA-256 is
`fbde6d1a070070c2ddf97f33f607edcf258ccbdbc2eaa34633bc61bb1511120c`.
That PyPI release is marked yanked, so HoshiGuard does not treat it as a
normative authority: Bridge, UBX, L1S, CAMF A1–A18 and EX extraction are
independently implemented and tested against the official documents. The
vendor code supplies the large DCR/CAMF/Japanese lookup tables and per-category
semantic calculations. Unknown values and vendor decoder failures retain raw
bits instead of becoming alerts with invented values.

Current limitations are deliberate:

- EX11 semantics depend on country-specific definitions that DCX-004 does not
  provide; all 68 bits are retained.
- A current authoritative JIS X 0402 dataset was not supplied or independently
  audited. Raw municipality codes survive; bundled names are a library snapshot.
- Real fixtures validate transport/PAB/MT/CRC and observed semantic values, but
  do not by themselves prove every possible DCR/DCX enumeration.
