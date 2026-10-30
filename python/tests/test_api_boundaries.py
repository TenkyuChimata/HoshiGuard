"""Synthetic edge cases and bounded-resource invariants for the public pipeline."""
import asyncio
from dataclasses import replace
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from aiohttp import web
from aiohttp.test_utils import TestClient, TestServer
from jsonschema import Draft202012Validator, FormatChecker

from hoshiguard.api import create_app
from hoshiguard.bridge_protocol import BridgeFrame, FrameType
from hoshiguard.events import EventProcessor
from hoshiguard.output.raw_archive import RawArchiveWriter
from hoshiguard.publisher import AlertStore
from hoshiguard.runtime import ReceiverPipeline
from test_api import real_alert, synthetic, NS, ROOT, SCHEMA
from test_dcr017 import bits, synthetic_dcr, wire_for_raw


def synthetic_dcx(*, action=1, hazard=8, version=1, country=111,
                  provider=3, onset=4612, mask=7, library=0):
    raw = bytearray(32)
    for offset, width, value in ((0, 8, 0x53), (8, 6, 44), (24, 2, action),
                                 (26, 9, country), (35, 5, provider), (40, 7, hazard),
                                 (47, 2, 3), (50, 14, onset), (67, 3, library),
                                 (147, 64, mask), (214, 6, version)):
        bits(raw, offset, width, value)
    return wire_for_raw(raw)


class BoundaryTests(unittest.TestCase):
    def test_dcx_schema_actions_and_training_realness(self):
        for code, action in ((0, 'issue'), (1, 'issue'), (2, 'update'), (3, 'clear')):
            store = AlertStore()
            record = ReceiverPipeline(store).process(synthetic_dcx(action=code), NS)
            value = json.loads(record.encoded)
            Draft202012Validator(SCHEMA, format_checker=FormatChecker()).validate(value)
            self.assertEqual(value['action'], action)
            self.assertEqual(value['payload']['type'], 'missile_attack')
            self.assertEqual(value['status'], 'test' if code == 0 else 'actual')
            self.assertEqual(value['is_real_disaster'], code != 0)

    def test_reserved_ex9_bits_never_become_prefectures(self):
        store = AlertStore()
        value = json.loads(ReceiverPipeline(store).process(synthetic_dcx(mask=7 | (1 << 63)), NS).encoded)
        self.assertEqual([a['code'] for a in value['areas']], [1, 2, 3])
        self.assertFalse(value['validity']['complete'])
        self.assertTrue(any(f['name'] == 'EX9_reserved' for f in value['validity']['unknown_fields']))
        self.assertEqual(value['payload']['extended']['fields']['EX9_target_area_code_list'], 7 | (1 << 63))
        self.assertEqual(value['payload']['extended']['fields']['EX9_target_area_code_list_hex'], '8000000000000007')

    def test_assumptive_hypocenter_markers_are_not_measurements(self):
        base = real_alert('qzss_l1s_mt43_pab53')
        raw = bytearray.fromhex(base.raw.l1s_hex)
        for offset, width, value in ((14, 3, 1), (17, 4, 1), (96, 9, 10),
                                     (105, 7, 10), (112, 10, 100),
                                     (122, 4, 10), (126, 4, 10)):
            bits(raw, offset, width, value)
        store = AlertStore()
        record = ReceiverPipeline(store).process(wire_for_raw(raw), NS)
        fields = json.loads(record.encoded)['payload']['fields']
        self.assertTrue(fields['assumptive'])
        self.assertIsNone(fields['magnitude'])
        self.assertIsNone(fields['depth_of_hypocenter'])
        self.assertEqual(fields['magnitude_raw'], 10)
        self.assertEqual(fields['depth_of_hypocenter_raw'], 10)

    def test_unimplemented_japan_version_not_published(self):
        store = AlertStore()
        self.assertIsNone(ReceiverPipeline(store).process(synthetic_dcx(version=2), NS))

    def test_ex11_unknown_country_version_is_raw_and_incomplete(self):
        store = AlertStore()
        value = json.loads(ReceiverPipeline(store).process(synthetic_dcx(country=10, provider=1, version=21), NS).encoded)
        self.assertEqual(value['payload']['extended']['variant'], 'outside_japan_ex11')
        self.assertFalse(value['validity']['complete'])
        self.assertTrue(value['payload']['limitations'])

    def test_unknown_dcr_code_is_retained_and_incomplete(self):
        store = AlertStore()
        record = ReceiverPipeline(store).process(synthetic_dcr(height=7), NS)
        value = json.loads(record.encoded)
        self.assertEqual(value['payload']['fields']['tsunami_heights_raw'], [7])
        self.assertFalse(value['validity']['complete'])
        self.assertEqual(value['validity']['unknown_fields'][0]['raw_value'], 7)

    def test_test_hazard_never_claims_real_disaster(self):
        for hazard in (13, 113):
            store = AlertStore()
            record = ReceiverPipeline(store).process(synthetic_dcx(hazard=hazard), NS)
            self.assertFalse(json.loads(record.encoded)['is_real_disaster'])
            self.assertEqual(record.status, 'test')

    def test_hello_heartbeat_does_not_reset_reassembly(self):
        pipeline = ReceiverPipeline(AlertStore())
        def hello(sequence):
            return BridgeFrame(1, 0, FrameType.HELLO, 0, sequence, 100, bytes(64), 0)
        pipeline.process(hello(0), NS)
        pipeline.reassembler.add(('synthetic',), 1, 2, b'A')
        pipeline.process(hello(1), NS)
        self.assertEqual(pipeline.reassembler.add(('synthetic',), 2, 2, b'B'), b'AB')
        pipeline.reassembler.add(('synthetic',), 1, 2, b'A')
        pipeline.process(hello(0), NS)
        self.assertIsNone(pipeline.reassembler.add(('synthetic',), 2, 2, b'B'))

    def test_no_disaster_does_not_fill_runtime_semantic_cache(self):
        from hoshiguard.output.raw_archive import RawArchiveReader
        fixture = ROOT / 'test/fixtures/bridge_real_session/selected_records.hgraw'
        pipeline = ReceiverPipeline(AlertStore(replay=True))
        for record in RawArchiveReader(str(fixture)):
            pipeline.process(record.frame, record.pc_receive_ns)
        self.assertEqual(len(pipeline.events.semantic), 1)

    def test_only_one_cross_thread_wakeup_can_be_pending(self):
        class Loop:
            calls = []
            def call_soon_threadsafe(self, callback):
                self.calls.append(callback)
        loop = Loop()
        store, processor, base = AlertStore(), EventProcessor(), real_alert()
        store.attach(loop)
        sub, _ = store.subscribe()
        for i in range(100):
            store.publish(processor.observe(synthetic(base, number=i)), NS)
        self.assertEqual(len(loop.calls), 1)
        self.assertTrue(sub.overflow)
        store.unsubscribe(sub)
        store.detach()

    def test_unknown_archive_never_appended(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'legacy.hgraw'
            original = b'unsupported historical evidence'
            path.write_bytes(original)
            with self.assertRaises(ValueError):
                RawArchiveWriter(str(path))
            self.assertEqual(path.read_bytes(), original)

    def test_public_examples_follow_schema(self):
        validator = Draft202012Validator(SCHEMA, format_checker=FormatChecker())
        for path in (ROOT / 'docs/examples').glob('*.json'):
            validator.validate(json.loads(path.read_text()))


class NetworkBoundaryTests(unittest.IsolatedAsyncioTestCase):
    async def test_timed_out_sender_isolated_and_unregistered(self):
        store = AlertStore()
        client = TestClient(TestServer(create_app(store, send_timeout=.01)))
        await client.start_server()
        async def blocked(*args, **kwargs):
            await asyncio.sleep(10)
        try:
            with patch.object(web.WebSocketResponse, 'send_str', blocked):
                ws = await client.ws_connect('/api/v1/ws')
                await asyncio.wait_for(ws.receive(), 1)
                await ws.close()
            for _ in range(100):
                if store.status()['clients'] == 0:
                    break
                await asyncio.sleep(.001)
            self.assertEqual(store.status()['clients'], 0)
            self.assertEqual((await client.get('/api/v1/status')).status, 200)
        finally:
            await client.close()

    async def test_sender_exception_isolated_without_leaking_details(self):
        store = AlertStore()
        client = TestClient(TestServer(create_app(store)))
        await client.start_server()
        async def failed(*args, **kwargs):
            raise RuntimeError('/private/synthetic-path')
        try:
            with patch.object(web.WebSocketResponse, 'send_str', failed), self.assertLogs('hoshiguard.api'):
                ws = await client.ws_connect('/api/v1/ws')
                message = await asyncio.wait_for(ws.receive(), 1)
                self.assertEqual(message.data, 1011)
                self.assertNotIn('/private', message.extra)
                await ws.close()
            self.assertEqual((await client.get('/api/v1/status')).status, 200)
            other = await client.ws_connect('/api/v1/ws')
            self.assertEqual((await other.receive_json())['type'], 'snapshot')
            await other.close()
        finally:
            await client.close()

    async def test_slow_subscription_does_not_affect_fast_client(self):
        store, processor, base = AlertStore(queue_capacity=2), EventProcessor(), real_alert()
        client = TestClient(TestServer(create_app(store)))
        await client.start_server()
        try:
            ws = await client.ws_connect('/api/v1/ws')
            await ws.receive_json()
            slow, _ = store.subscribe()
            for i in range(10):
                record = store.publish(processor.observe(synthetic(base, number=i)), NS)
                self.assertEqual((await ws.receive_json())['data']['id'], record.id)
            self.assertTrue(store.pop(slow)[0])
            store.unsubscribe(slow)
            await ws.close()
        finally:
            await client.close()

    async def test_offset_pages_and_reconnect_snapshot_truncation(self):
        store, processor, base = AlertStore(), EventProcessor(), real_alert()
        for i in range(105):
            store.publish(processor.observe(synthetic(base, number=i)), NS)
        client = TestClient(TestServer(create_app(store)))
        await client.start_server()
        try:
            ws = await client.ws_connect('/api/v1/ws')
            snapshot = await ws.receive_json()
            self.assertTrue(snapshot['data']['truncated'])
            first = await (await client.get('/api/v1/alerts?limit=100')).json()
            second = await (await client.get('/api/v1/alerts?offset=100')).json()
            self.assertEqual(len(first['data']) + len(second['data']), 105)
            self.assertEqual(first['sequence'], second['sequence'])
            self.assertFalse(set(x['id'] for x in first['data']) & set(x['id'] for x in second['data']))
            await ws.close()
        finally:
            await client.close()


if __name__ == '__main__':
    unittest.main()
