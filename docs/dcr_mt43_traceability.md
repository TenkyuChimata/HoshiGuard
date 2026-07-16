# MT43 traceability — IS-QZSS-DCR-016

All fields use MSB-first L1S bit numbering. Common fields are decoded in the
vendored, MIT-licensed per-category decoders and wrapped by
`python/hoshiguard/dcr/decoder.py`.

| Common field | Bits | Width | Model/output |
|---|---:|---:|---|
| report classification | 14–17 | 3 | raw number + JA/EN name/status |
| disaster category | 17–21 | 4 | typed category payload + raw code |
| report month/day/hour/minute | 21–41 | 20 | raw components + inferred year |
| information type | 41–43 | 2 | issue/correction/cancellation |
| DCR version | 214–220 | 6 | raw version |
| CRC | 226–250 | 24 | independent L1S verifier |

| Code | DCR-016 payload | Python model/decoder | Tables | Coverage |
|---:|---|---|---|---|
| 1 | Earthquake Early Warning | `EarthquakeEarlyWarningPayload` / `qzss_dcr_decoder_jma_earthquake_early_warning.py` | intensity, epicenter, EEW regions | semantic core only |
| 2 | Hypocenter | `HypocenterPayload` / `..._hypocenter.py` | epicenter, magnitude/depth specials | real + semantic core |
| 3 | Seismic Intensity | `SeismicIntensityPayload` / `..._seismic_intensity.py` | prefecture/intensity | semantic core |
| 4 | Nankai Trough | `NankaiTroughPayload` / `..._nankai_trough_earthquake.py` | information serial; 18-byte pages | bounded reassembly test |
| 5 | Tsunami | `TsunamiPayload` / `..._tsunami.py` | coastal region/height/warning | semantic core |
| 6 | NW Pacific Tsunami | `NorthwestPacificTsunamiPayload` / `..._northwest_pacific_tsunami.py` | potential/height | semantic core |
| 8 | Volcano | `VolcanoPayload` / `..._volcano.py` | volcano/local government/warning | semantic core |
| 9 | Ash Fall | `AshFallPayload` / `..._ash_fall.py` | ash warning/local government | semantic core |
| 10 | Weather | `WeatherPayload` / `..._weather.py` | warning state/forecast region | semantic core |
| 11 | Flood | `FloodPayload` / `..._flood.py` | flood level/region | semantic core |
| 12 | Typhoon | `TyphoonPayload` / `..._typhoon.py` | scale/intensity/reference | real + semantic core |
| 14 | Marine | `MarinePayload` / `..._marine.py` | warning/forecast region | real capture + semantic core |

Codes 7 and 13 are unused; raw codes remain visible. Per-payload offsets,
scales, units and special values are implemented in the named decoder plus its
definition file and traced to DCR-016 section 4.1.2 category tables. The older
DCR-013 application note is excluded from offset decisions.
