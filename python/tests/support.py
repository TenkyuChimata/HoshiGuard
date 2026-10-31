"""実受信の読み込みと合成入力。生成した値は実放送の証拠としない。"""
from dataclasses import replace
import json
from pathlib import Path
from hoshiguard.bridge_frames import L1sFrame, parse_payload
from hoshiguard.bridge_protocol import BridgeFrame, FrameType, decode_wire, encode_frame
from hoshiguard.events import EventProcessor
from hoshiguard.events.canonical import semantic_fingerprint
from hoshiguard.l1s import crc24, read_bits, verify_ubx_l1s
from hoshiguard.output.raw_archive import RawArchiveReader
from hoshiguard.routing import make_alert
from hoshiguard.ubx import checksum


ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / 'test/fixtures/bridge_real_session/selected_records.hgraw'
NS = 1784097827645520000
SCHEMA = json.loads((ROOT / 'docs/public-alert.v1.schema.json').read_text())
MANIFEST = json.loads((ROOT / 'test/fixtures/real/manifest.json').read_text())

def fixture(name):
    return next(item for item in MANIFEST["fixtures"] if item["name"] == name)

def claim_for(item):
    verified = verify_ubx_l1s(bytes.fromhex(item["ubx_hex"]))
    return verified, L1sFrame(0, verified.sfrbx.sv_id, 5, 1,
                              verified.sfrbx.num_words, verified.pab,
                              verified.message_type, 0x0F,
                              verified.sfrbx.packet.raw, 0, 0, "0.2.5")

def alert_for(name, sequence=1, timestamp=100):
    verified = verify_ubx_l1s(bytes.fromhex(fixture(name)["ubx_hex"]))
    claim = L1sFrame(1, verified.sfrbx.sv_id, verified.sfrbx.gnss_id,
                    verified.sfrbx.sig_id, verified.sfrbx.num_words,
                    verified.pab, verified.message_type, 0x0F,
                    verified.sfrbx.packet.raw, 0, 0, "0.2.6")
    bridge = BridgeFrame(1, 0, FrameType.L1S_FRAME, 0, sequence, timestamp,
                         b"", 0)
    return make_alert(bridge, claim, verified)

def real_marine_alert():
    path = ROOT / "test/fixtures/bridge_real_session/selected_records.hgraw"
    for record in RawArchiveReader(str(path)):
        if record.frame.frame_type != FrameType.L1S_FRAME:
            continue
        claim = parse_payload(record.frame)
        if claim.message_type == 43:
            verified = verify_ubx_l1s(claim.ubx)
            alert = make_alert(record.frame, claim, verified)
            if alert.category.code == 14:
                return alert
    raise AssertionError("selected real session fixture lacks Marine MT43")

def real_alert(name=None):
    if name is None:
        for record in RawArchiveReader(str(FIXTURE)):
            if record.frame.frame_type == FrameType.L1S_FRAME:
                claim = parse_payload(record.frame)
                frame = verify_ubx_l1s(claim.ubx)
                if frame.message_type == 43:
                    return make_alert(record.frame, claim, frame)
        raise AssertionError('missing real DCR fixture')
    manifest = json.loads((ROOT / 'test/fixtures/real/manifest.json').read_text())
    item = next(item for item in manifest['fixtures'] if item['name'] == name)
    frame = verify_ubx_l1s(bytes.fromhex(item['ubx_hex']))
    claim = L1sFrame(0, frame.sfrbx.sv_id, 5, 1, frame.sfrbx.num_words,
                     frame.pab, frame.message_type, 15, frame.sfrbx.packet.raw, 0, 0, 'fixture')
    return make_alert(BridgeFrame(1, 0, FrameType.L1S_FRAME, 0, 1, 100, b'', 0), claim, frame)

def synthetic(base=None, *, action='issue', status='actual', number=0):
    base = base or real_alert()
    # キャッシュ済み hash だけでなく、仕様上の payload 値を明示的に変更する。
    fields = dict(base.payload.fields)
    fields['synthetic_test_discriminator'] = number
    alert = replace(base, action=action, status=status,
                    payload=replace(base.payload, fields=tuple(sorted(fields.items()))))
    return replace(alert, identity=replace(alert.identity, semantic_fingerprint=semantic_fingerprint(alert)))

def alert():
    item = next(x for x in MANIFEST["fixtures"]
                if x["name"] == "qzss_l1s_mt43_pab53")
    verified = verify_ubx_l1s(bytes.fromhex(item["ubx_hex"]))
    claim = L1sFrame(1, verified.sfrbx.sv_id, verified.sfrbx.gnss_id,
                    verified.sfrbx.sig_id, verified.sfrbx.num_words,
                    verified.pab, verified.message_type, 0x0F,
                    verified.sfrbx.packet.raw, 4, 2, "0.2.7")
    frame = BridgeFrame(1, 0, FrameType.L1S_FRAME, 0, 1, 100, b"", 0)
    return make_alert(frame, claim, verified)

def event_result():
    return EventProcessor().observe(alert())

def wire_for_raw(raw):
    bits(raw, 226, 24, crc24(raw))
    words = b''.join(raw[i:i+4][::-1] for i in range(0, 32, 4))
    payload = bytes((5, 3, 1, 0, 8, 0, 2, 0)) + words
    ubx = b'\xb5\x62\x02\x13' + len(payload).to_bytes(2, 'little') + payload
    ubx += bytes(checksum(ubx[2:]))
    metadata = bytearray(20)
    metadata[4:10] = bytes((3, 5, 1, 8, raw[0], read_bits(raw, 8, 6)))
    metadata[10:12] = (15).to_bytes(2, 'little')
    metadata[12:14] = len(ubx).to_bytes(2, 'little')
    return decode_wire(encode_frame(FrameType.L1S_FRAME, 1, 1, bytes(metadata) + ubx))

def synthetic_dcr(category=5, *, arrival=(0, 30, 62), height=13,
                  classification=3, information=0, warning=3, region=100):
    raw = bytearray(32)
    for offset, width, value in [(0, 8, 0x53), (8, 6, 43), (14, 3, classification),
                                 (17, 4, category), (21, 4, 7), (25, 5, 15),
                                 (30, 5, 2), (35, 6, 35), (41, 2, information), (214, 6, 1)]:
        bits(raw, offset, width, value)
    if category == 5:
        for offset, width, value in [(80, 4, warning), (84, 1, arrival[0]), (85, 5, arrival[1]),
                                     (90, 6, arrival[2]), (96, 4, height), (100, 10, region)]:
            bits(raw, offset, width, value)
    elif category == 11:
        bits(raw, 53, 4, warning)
        bits(raw, 57, 40, region)
    elif category == 10:
        bits(raw, 53, 3, warning)
        bits(raw, 56, 5, 1)
        bits(raw, 61, 19, 11000)
    return wire_for_raw(raw)

def set_bits(data: bytearray, offset: int, width: int, value: int) -> None:
    if value < 0 or value >= (1 << width):
        raise ValueError("value does not fit")
    for index in range(width):
        bit = (value >> (width - index - 1)) & 1
        position = offset + index
        mask = 1 << (7 - (position & 7))
        if bit:
            data[position >> 3] |= mask
        else:
            data[position >> 3] &= ~mask

def synthetic_dcx(*, action=1, hazard=8, version=1, country=111,
                  provider=3, onset=4612, mask=7, library=0):
    raw = bytearray(32)
    for offset, width, value in ((0, 8, 0x53), (8, 6, 44), (24, 2, action),
                                 (26, 9, country), (35, 5, provider), (40, 7, hazard),
                                 (47, 2, 3), (50, 14, onset), (67, 3, library),
                                 (147, 64, mask), (214, 6, version)):
        bits(raw, offset, width, value)
    return wire_for_raw(raw)

bits = set_bits
