# Evidence provenance

The 24-hour archive directly validates firmware **0.2.5**, build
`Jul 15 2026 16:53:42`, Bridge protocol 1.0. It must not be described as a
24-hour run of firmware 0.2.6 or 0.2.7.

The v0.2.6 STM32 change was the firmware version text; semantic identity,
session validation and DCR scenario work were host-side Python changes. v0.2.7
also leaves COBS, CRC, wire header/payloads, queue, L1S filter, STATUS,
TIME_STATUS, PPS and UART Bridge output unchanged. The 0.2.5 session therefore
provides inherited evidence for protocol 1.0 and that unchanged data path.

It does **not** prove that the newly built 0.2.7 binary, toolchain output,
deployment configuration or host rotation logic has run for 24 hours on real
hardware. Confirm HELLO `firmware=0.2.7`, perform a short current-image hardware
regression, and repeat the soak when certification must name the 0.2.7 image.

The historical 0.2.5 source commit is not present in this workspace. The
equivalence statement is consequently bounded to the retained source/change
audit, unchanged Bridge tests and identical v0.2.5/v0.2.6 resource footprint;
it is not represented as a cryptographic source-tree comparison.
