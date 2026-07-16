# MT44 traceability — IS-QZSS-DCX-004 / CAMF 1.2

Bit zero is the MSB of PAB. The ranges below are half-open `[offset,end)`.
They are centralized in `python/hoshiguard/dcx/camf.py`; there are no duplicate
decoder magic offsets.

| Field | Bits | Width | Encoding/model | Test |
|---|---:|---:|---|---|
| SD mask type/mask | 14 / 15 | 1 / 9 | raw transport designation | real MT44 |
| A1 Message Type | 24–26 | 2 | `CamfFields.a1_message_type` | A1–A18 coverage |
| A2 Country/Region | 26–35 | 9 | raw + CAMF table | real/synthetic |
| A3 Provider | 35–40 | 5 | provider depends on A2 | scenario 1–3 |
| A4 Hazard | 40–47 | 7 | versioned 1–113 table | all 113 test |
| A5 Severity | 47–49 | 2 | raw 0–3 + table | scenario manifest |
| A6 Onset week | 49–50 | 1 | raw + inferred time kept separate | decoder tests |
| A7 Onset TOW | 50–64 | 14 | minutes/week, raw preserved | decoder tests |
| A8 Duration | 64–66 | 2 | CAMF duration class | decoder tests |
| A9 Library | 66–67 | 1 | international/country | decoder tests |
| A10 Version | 67–70 | 3 | library version | decoder tests |
| A11 Guidance | 70–80 | 10 | library lookup + raw | scenario 2/3 |
| A12 Latitude | 80–96 | 16 | CAMF scaled ellipse latitude | geometry tests |
| A13 Longitude | 96–113 | 17 | CAMF scaled ellipse longitude | geometry tests |
| A14 Semi-major | 113–118 | 5 | CAMF axis table | geometry tests |
| A15 Semi-minor | 118–123 | 5 | CAMF axis table | geometry tests |
| A16 Azimuth | 123–129 | 6 | CAMF scale | geometry tests |
| A17 Specific kind | 129–131 | 2 | B1/B2/B3/B4 | vendor + trace test |
| A18 Specific data | 131–146 | 15 | C/D fields selected by A17/A4 | vendor tests |
| EX1 | 146–162 | 16 | target area code | local/L-Alert |
| EX2 | 162–163 | 1 | evacuation direction | local government |
| EX3 | 163–180 | 17 | additional latitude | local government |
| EX4 | 180–197 | 17 | additional longitude | local government |
| EX5 | 197–202 | 5 | additional semi-major | local government |
| EX6 | 202–207 | 5 | additional semi-minor | local government |
| EX7 | 207–214 | 7 | additional azimuth | local government |
| EX8 | 146–147 | 1 | J-Alert list type | J-Alert |
| EX9 | 147–211 | 64 | prefecture mask or four codes | J-Alert |
| EX10 | 211–214 | 3 | reserved, preserved | reserved test |
| EX11 | 146–214 | 68 | raw country-specific extension | blocked/raw test |
| Vn | 214–220 | 6 | DCX version | real MT44 |

The semantic table/calculation implementation is in the attributed vendor
tree and is wrapped by `dcx/decoder.py`; Human and NDJSON consume only
`UnifiedAlert`. Real fixture coverage locks PAB/MT/CRC and observed fields.
The July 2026 scenario PDF supplies coverage expectations, never these offsets.

