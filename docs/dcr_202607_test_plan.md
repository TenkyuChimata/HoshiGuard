# DCR official training/test plan — July 2026

Source `details_202607.pdf` is an official scenario oracle only. DCR-016 stays
normative for bits and semantics. No broadcast binary is fabricated.

Windows: 2026-07-14 13:00–15:00 JST (04:00–06:00 UTC) and 2026-07-30
10:00–12:00 JST (01:00–03:00 UTC). Test messages use Rc=7. Highest-priority
actual traffic may replace test slots; priority/regular actual traffic may be
interleaved, so `not_received` is not `decode_failed`.

Seven scenarios cover nine categories:

1. EEW, hypocenter, intensity, tsunami issue.
2. Tsunami update with an added forecast region.
3. Nankai Trough text and tsunami clear.
4. Volcano and rapid ash forecast.
5. Detailed ash: five messages, at most four locations each, 18 ordered
   location/hour/warning triples, Rc through volcano common.
6. Weather: three parts/14 entries sorted by subtype then region; flood issue.
7. Flood clear for the same raw forecast-region code.

Nankai text is 158 characters / 474 UTF-8 bytes, six decoded characters per
page. Expected Pn is 1..27 and Pm=27. Validation accepts arbitrary arrival
order, retains raw pages, detects duplicate/conflicting/missing pages and only
decodes UTF-8 after page ordering.

For the 2026-07-30 run, start before 10:00 JST and stop after 12:00 JST:

```sh
pio run -e genericSTM32F103C8-bridge-binary -t upload
PYTHONPATH=python python3 -m hoshiguard \
  --port /dev/ttyUSB3 --baud 115200 --all-observations \
  --raw-archive captures/dcr-20260730.hgraw \
  --ndjson captures/dcr-20260730-all.ndjson

python3 tools/validate_bridge_session.py captures/dcr-20260730.hgraw \
  --json-report reports/dcr-20260730-bridge.json
python3 tools/validate_dcr_test_scenario.py captures/dcr-20260730.hgraw \
  --manifest test/fixtures/dcr_202607/scenario_manifest.json \
  --json-report reports/dcr-20260730-scenario.json
```

Keep the raw archive even if semantic/event streams suppress repeats.
