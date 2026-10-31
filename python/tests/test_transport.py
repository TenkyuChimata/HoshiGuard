"""Bridge 1.0、独立 UBX/L1S 検証、証跡復旧とシリアル再接続。"""
from support import MANIFEST
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest
from unittest.mock import patch
from hoshiguard.bridge_frames import Hello, L1sFrame, Status, TimeStatus, parse_payload
from hoshiguard.bridge_protocol import (
    BridgeFrame,
    CobsError,
    FrameType,
    SequenceDisposition,
    SequenceTracker,
    StreamDecoder,
    cobs_decode,
    cobs_encode,
    crc32_iso_hdlc,
    decode_wire,
    encode_frame,
)
from hoshiguard.l1s import compare_claim, verify_ubx_l1s
from hoshiguard.output.raw_archive import RawArchiveReader, RawArchiveWriter
from hoshiguard.serial_receiver import SerialReceiver


REAL = next(item for item in MANIFEST["fixtures"] if item["name"] == "qzss_l1s_mt44_pab53")


def l1s_wire(sequence=1):
    ubx = bytes.fromhex(REAL["ubx_hex"])
    verified = verify_ubx_l1s(ubx)
    payload = bytearray(20)
    payload[4:10] = bytes((verified.sfrbx.sv_id, 5, 1, 9,
                           verified.pab, verified.message_type))
    payload[10:12] = (0x0F).to_bytes(2, "little")
    payload[12:14] = len(ubx).to_bytes(2, "little")
    payload[16:19] = bytes((0, 2, 5))
    payload.extend(ubx)
    return encode_frame(FrameType.L1S_FRAME, sequence, 123456789, bytes(payload))


class BridgeProtocolTests(unittest.TestCase):
    def test_crc_check(self):
        self.assertEqual(crc32_iso_hdlc(b"123456789"), 0xCBF43926)

    def test_cobs_round_trip_all_byte_shapes(self):
        for value in (b"", b"\0", b"a\0b", bytes(range(256)), b"x" * 508):
            self.assertEqual(cobs_decode(cobs_encode(value)), value)

    def test_invalid_cobs(self):
        with self.assertRaises(CobsError):
            cobs_decode(b"\0")
        with self.assertRaises(CobsError):
            cobs_decode(b"\x05a")

    def test_fragmented_and_multiple_reads(self):
        wires = l1s_wire(1) + encode_frame(FrameType.HEARTBEAT, 2, 9, b"\1\0\0\0")
        decoder = StreamDecoder()
        frames = []
        for byte in wires:
            frames.extend(decoder.feed(bytes((byte,))))
        self.assertEqual([frame.sequence for frame in frames], [1, 2])

    def test_corrupt_frame_recovers_at_next_delimiter(self):
        bad = bytearray(l1s_wire(1)); bad[5] ^= 0x40
        decoder = StreamDecoder()
        frames = decoder.feed(bytes(bad) + l1s_wire(2))
        self.assertEqual([frame.sequence for frame in frames], [2])
        self.assertGreaterEqual(decoder.stats.cobs_errors + decoder.stats.crc_errors, 1)

    def test_unsupported_major_is_rejected_but_newer_minor_is_accepted(self):
        unsupported = BridgeFrame(2, 0, FrameType.HEARTBEAT, 0, 1, 1,
                                  b"\0\0\0\0", 0).wire_bytes()
        decoder = StreamDecoder()
        self.assertEqual(decoder.feed(unsupported), [])
        self.assertEqual(decoder.stats.version_errors, 1)
        compatible = BridgeFrame(1, 9, 99, 0, 2, 2, b"future", 0).wire_bytes()
        frame = decoder.feed(compatible)[0]
        self.assertEqual((frame.minor, frame.frame_type, frame.payload),
                         (9, 99, b"future"))

    def test_oversized_record_recovers(self):
        decoder = StreamDecoder(maximum=80)
        frames = decoder.feed(b"A" * 100 + b"\0" +
                              encode_frame(FrameType.HEARTBEAT, 2, 9, b""))
        self.assertEqual(frames[0].sequence, 2)
        self.assertEqual(decoder.stats.oversized, 1)

    def test_sequence_gap_duplicate_out_of_order_rollover(self):
        tracker = SequenceTracker()
        self.assertEqual(tracker.observe(0xFFFFFFFE).disposition, SequenceDisposition.FIRST)
        self.assertEqual(tracker.observe(0xFFFFFFFF).disposition, SequenceDisposition.CONTIGUOUS)
        self.assertEqual(tracker.observe(0).disposition, SequenceDisposition.CONTIGUOUS)
        self.assertEqual(tracker.observe(2).missing, 1)
        self.assertEqual(tracker.observe(2).disposition, SequenceDisposition.DUPLICATE)
        self.assertEqual(tracker.observe(1).disposition, SequenceDisposition.OUT_OF_ORDER)

    def test_hello_creates_restart_epoch(self):
        tracker = SequenceTracker(); tracker.observe(100)
        self.assertEqual(tracker.observe(0, hello=True).disposition,
                         SequenceDisposition.NEW_EPOCH)

    def test_typed_l1s_and_independent_verification(self):
        frame = decode_wire(l1s_wire())
        claim = parse_payload(frame)
        comparison = compare_claim(claim)
        self.assertFalse(comparison.mismatch)
        self.assertTrue(comparison.verified.valid_crc)
        self.assertEqual(comparison.verified.extra_word_ubx.hex().upper(),
                         REAL["extra_words_hex"])

    def test_typed_hello_and_status_payloads(self):
        hello_payload = bytearray(64)
        hello_payload[:8] = bytes((1, 0, 0, 2, 5, 1, 1, 1))
        hello_payload[8:24] = struct.pack("<IIIHH", 0x1F, 115200, 38400,
                                           264, 384)
        board = b"STM32F103C8"
        hello_payload[24:24 + len(board)] = board
        hello = parse_payload(decode_wire(encode_frame(
            FrameType.HELLO, 0, 1, bytes(hello_payload))))
        self.assertIsInstance(hello, Hello)
        self.assertEqual((hello.firmware, hello.gnss_baud), ("0.2.5", 38400))

        status_payload = bytearray(88)
        status_payload[:12] = struct.pack("<IIBBBB", 1000, 115200, 9, 4, 5, 6)
        status_payload[12:] = struct.pack("<19I", *range(1, 20))
        status = parse_payload(decode_wire(encode_frame(
            FrameType.STATUS, 1, 2, bytes(status_payload))))
        self.assertIsInstance(status, Status)
        self.assertEqual((status.ubx_received, status.bridge_sent,
                          status.l1s_dropped, status.encode_errors),
                         (1, 8, 18, 19))

        time_payload = bytearray(32)
        time_payload[:14] = struct.pack("<IBBBB iBB", 8, 4, 5, 12, 0x13,
                                        -42, 1, 0x24)
        time_payload[14:28] = struct.pack("<HIII", 2410, 345000, 123, 8)
        time_payload[28] = 0x05
        timing = parse_payload(decode_wire(encode_frame(
            FrameType.TIME_STATUS, 2, 3, bytes(time_payload))))
        self.assertIsInstance(timing, TimeStatus)
        self.assertEqual((timing.week, timing.tow_ms, timing.q_err_ps),
                         (2410, 345000, -42))
        self.assertTrue(timing.matched)
        self.assertFalse(timing.ambiguous)
        self.assertTrue(timing.time_grid_known)

    def test_claim_mismatch_is_detected(self):
        claim = parse_payload(decode_wire(l1s_wire()))
        bad = L1sFrame(**{**claim.__dict__, "message_type": 43})
        self.assertTrue(compare_claim(bad).mismatch)

    def test_raw_archive_roundtrip_and_recovery(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.hgraw"
            with RawArchiveWriter(str(path), flush_every=1) as writer:
                writer.write(l1s_wire(1), 100)
                writer.write(l1s_wire(2), 200)
            records = list(RawArchiveReader(str(path)))
            self.assertEqual([record.frame.sequence for record in records], [1, 2])
            data = bytearray(path.read_bytes()); data[45] ^= 1; path.write_bytes(data)
            reader = RawArchiveReader(str(path)); recovered = list(reader)
            self.assertEqual([record.frame.sequence for record in recovered], [2])
            self.assertEqual(reader.corrupt_records, 1)

    def test_serial_disconnect_discards_partial_and_starts_new_hello_epoch(self):
        hello_payload = bytearray(64)
        hello_payload[0], hello_payload[6], hello_payload[7] = 1, 1, 1
        hello = encode_frame(FrameType.HELLO, 0, 1, bytes(hello_payload))

        class SerialException(Exception):
            pass

        class Connection:
            def __init__(self, reads):
                self.reads = list(reads)
                self.closed = False

            def read(self, _size):
                item = self.reads.pop(0)
                if isinstance(item, Exception):
                    raise item
                return item

            def close(self):
                self.closed = True

        connections = [Connection((hello, b"\x02\x03", SerialException())),
                       Connection((hello,))]
        fake_serial = types.SimpleNamespace(
            Serial=lambda *_args, **_kwargs: connections.pop(0),
            SerialException=SerialException,
        )
        statuses = []
        receiver = SerialReceiver("fake", reconnect_delay=0,
                                  status_callback=statuses.append)
        with patch.dict(sys.modules, {"serial": fake_serial}):
            stream = receiver.frames()
            first, _ = next(stream)
            second, _ = next(stream)
            stream.close()
        self.assertEqual((first.sequence, second.sequence), (0, 0))
        self.assertEqual(receiver.stats.disconnects, 1)
        self.assertEqual(receiver.stats.reconnects, 2)
        self.assertEqual(receiver.stats.epochs, 1)
        self.assertEqual(receiver.decoder.stats.partial_discarded, 1)
        self.assertTrue(any("disconnected" in item for item in statuses))

if __name__ == "__main__":
    unittest.main()
