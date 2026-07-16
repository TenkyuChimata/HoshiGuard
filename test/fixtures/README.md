# Real capture fixtures

`real/manifest.json` and the generated headers come from the checksum-valid
RXM-SFRBX records in `capture-outdoor.log` (MAX-M10S SPG 5.10, 2026-07-14).
They retain no NAV-PVT packet, coordinate, precise location, identity, or USB
serial number. The manifest records `region: withheld` and
`coordinates: not retained`.

Regenerate and verify deterministically with:

```sh
python3 tools/import_real_fixtures.py capture-outdoor.log
python3 tools/import_real_fixtures.py capture-outdoor.log --check
```

Selection is the first checksum/PAB/CRC-valid complete UBX record by timestamp
for each requested MT/PAB. `qzss_l1ca_negative` is also real. The non-QZSS,
checksum-corrupted and SFRBX-length-mismatch negatives are explicitly marked
synthetic-derived/corrupted and must never be described as satellite frames.
`manifest.json` is the per-fixture source document: it includes capture SHA-256,
timestamp, svId/sigId/numWords, PAB/MT, received/calculated CRC, duplicate flag,
packet hash and semantic-review limitations.
