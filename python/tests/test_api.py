"""API テスト: 実 fixture 再生と、合成であることを明示したライフサイクル変種。

合成した変更は衛星放送の実受信証拠ではない。
ネットワーク検証は loopback に bind し、ローカル socket を許可する実行環境が必要。
"""
from support import (
    ROOT,
    FIXTURE,
    NS,
    SCHEMA,
    real_alert,
    synthetic,
    bits,
    wire_for_raw,
    synthetic_dcr,
    synthetic_dcx,
)
import asyncio
from dataclasses import replace
import json
from pathlib import Path
import threading
import time
import unittest
from unittest.mock import patch
from aiohttp import WSMsgType, WSServerHandshakeError
from aiohttp.test_utils import TestClient, TestServer
from jsonschema import Draft202012Validator, FormatChecker
from hoshiguard.api import create_app
from hoshiguard.bridge_protocol import BridgeFrame, FrameType
from hoshiguard.events import EventProcessor
from hoshiguard.models import json_safe
from hoshiguard.output.raw_archive import RawArchiveReader
from hoshiguard.public import PublicAlert
from hoshiguard.publisher import AlertStore
from hoshiguard.routing import make_alert
from hoshiguard.runtime import ReceiverConfig, ReceiverPipeline, receive
import tempfile
from aiohttp import web
from hoshiguard.output.raw_archive import RawArchiveWriter
import socket
import subprocess
import sys
import urllib.error
import urllib.request


class PublicStateTests(unittest.TestCase):
    def test_public_schema_and_privacy_real_fixture(self):
        value = json_safe(PublicAlert.from_alert(real_alert(), NS))
        Draft202012Validator(SCHEMA, format_checker=FormatChecker()).validate(value)
        self.assertIsNone(value['identity']['event_id'])
        self.assertTrue(value['times']['issued'].endswith('+00:00'))
        self.assertEqual(value['times']['onset'], None)
        self.assertIn('marine', value['payload']['type'])
        self.assertTrue(value['areas'])
        for key in ('ubx_hex', 'l1s_hex', 'bridge_sequence', 'timestamp_us', 'pps_sequence', 'serial_port'):
            self.assertNotIn(key, json.dumps(value))

    def test_unknown_missing_measurements_are_not_zero(self):
        value = json_safe(PublicAlert.from_alert(real_alert(), NS))
        for key in ('magnitude', 'depth', 'hypocenter', 'intensity'):
            self.assertNotIn(key, value)
        self.assertIn('marine_warning_codes', value['severity'])
        self.assertIsNone(value['times']['expires'])

    def test_non_alert_slas_and_null_rejected(self):
        store, events = AlertStore(), EventProcessor()
        for name in ('qzss_l1s_mt47', 'qzss_l1s_mt44_pab53'):
            alert = real_alert(name)
            self.assertIsNone(store.publish(events.observe(alert), NS))
            with self.assertRaises(ValueError):
                PublicAlert.from_alert(alert, NS)
        self.assertEqual(store.query()[0], [])

    def test_validation_failure_not_published(self):
        base = real_alert()
        bad = replace(base, validity=replace(base.validity, l1s_crc=False))
        self.assertIsNone(AlertStore().publish(EventProcessor().observe(bad), NS))

    def test_parallel_reports_with_same_limited_hint_stay_independent(self):
        events, store = EventProcessor(), AlertStore()
        for i in range(3):
            result = events.observe(synthetic(number=i))
            self.assertEqual(result.disposition, 'issued')
            store.publish(result, NS)
        self.assertEqual(len(store.query(states=True)[0]), 3)
        self.assertEqual(len(events.events), 3)

    def test_capacity_and_ttl_bound_both_histories(self):
        now = [0.0]
        store, events = AlertStore(capacity=4, ttl_seconds=10, clock=lambda: now[0]), EventProcessor(capacity=4)
        base = real_alert()
        for i in range(1000):
            now[0] += .001
            store.publish(events.observe(synthetic(base, number=i)), NS)
        self.assertEqual(store.status()['alerts_cached'], 4)
        self.assertEqual(store.status()['states_cached'], 4)
        self.assertLessEqual(len(events.semantic), 4)
        now[0] = 100
        self.assertEqual(store.query()[0], [])
        self.assertEqual(store.status()['states_cached'], 0)

    def test_device_time_rollback_does_not_break_runtime_ttl(self):
        base = real_alert()
        events = EventProcessor(ttl_us=10)
        events.observe(base, now_us=100)
        rollback = replace(base, receipt=replace(base.receipt, timestamp_us=1))
        self.assertEqual(events.observe(rollback, now_us=105).disposition, 'repeated')
        self.assertEqual(events.observe(rollback, now_us=120).repeat_count, 1)

    def test_health_quiet_no_signal_disconnect_and_historical(self):
        now = [0.0]
        store = AlertStore(clock=lambda: now[0])
        store.set_receiver('connected', connected=True)
        store.frame_received(NS, l1s=True)
        now[0] = 120
        store.frame_received(NS)  # heartbeat は継続しているが、起動後の L1S はない
        self.assertEqual(store.status()['signal'], 'no_l1s')
        self.assertEqual(store.status()['decode_errors'], 0)
        store.frame_received(NS, l1s=True)
        self.assertEqual(store.status()['data_mode'], 'live')
        now[0] += 20
        self.assertEqual(store.status()['link'], 'stalled')
        store.set_receiver('disconnected')
        self.assertEqual(store.status()['data_mode'], 'historical')
        self.assertEqual(store.status()['link'], 'disconnected')

    def test_restart_has_new_process_scope_and_empty_cache(self):
        first = AlertStore()
        first.publish(EventProcessor().observe(real_alert()), NS)
        second = AlertStore()
        self.assertNotEqual(first.process_id, second.process_id)
        self.assertEqual(second.sequence, 0)
        self.assertEqual(second.query()[0], [])

    def test_slow_subscription_overflow_is_bounded(self):
        store = AlertStore(queue_capacity=2)
        sub, _ = store.subscribe()
        events, base = EventProcessor(), real_alert()
        for i in range(20):
            store.publish(events.observe(synthetic(base, number=i)), NS)
        self.assertTrue(store.pop(sub)[0])
        self.assertLessEqual(len(sub.messages), 2)
        self.assertEqual(store.status()['alerts_cached'], 20)
        store.unsubscribe(sub)
        self.assertEqual(store.status()['clients'], 0)

    def test_real_hgraw_end_to_end_with_raw_preservation(self):
        import tempfile
        with tempfile.TemporaryDirectory() as temp:
            archive = str(Path(temp) / 'preserved.hgraw')
            store = AlertStore(replay=True)
            receive(ReceiverConfig(replay=str(FIXTURE), raw_archive=archive), store, threading.Event())
            self.assertEqual(len(list(RawArchiveReader(archive))), len(list(RawArchiveReader(str(FIXTURE)))))
            self.assertEqual(store.status()['receiver'], 'replay_complete')
            self.assertEqual(store.status()['data_mode'], 'historical')
            self.assertEqual(store.status()['verification_errors'], 0)
            self.assertEqual(store.status()['decode_errors'], 0)
            self.assertTrue(store.query()[0])

    def test_decode_failure_keeps_raw_and_next_report(self):
        import tempfile
        with tempfile.TemporaryDirectory() as temp:
            archive = str(Path(temp) / 'evidence.hgraw')
            store = AlertStore(replay=True)
            original = make_alert
            calls = [0]
            def fail_first(*args, **kwargs):
                calls[0] += 1
                if calls[0] == 1:
                    from hoshiguard.dcr.decoder import DcrDecodeError
                    raise DcrDecodeError('synthetic failure /private/file')
                return original(*args, **kwargs)
            with patch('hoshiguard.runtime.make_alert', side_effect=fail_first), self.assertLogs('hoshiguard.runtime'):
                receive(ReceiverConfig(replay=str(FIXTURE), raw_archive=archive), store, threading.Event())
            self.assertEqual(store.status()['decode_errors'], 1)
            self.assertNotIn('/private', json.dumps(store.status()))
            self.assertEqual(len(list(RawArchiveReader(archive))), len(list(RawArchiveReader(str(FIXTURE)))))
            self.assertTrue(store.query()[0])


class ApiTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.store = AlertStore()
        self.events = EventProcessor()
        self.base = real_alert()
        self.client = TestClient(TestServer(create_app(self.store)))
        await self.client.start_server()

    async def asyncTearDown(self):
        await self.client.close()

    def post(self, alert=None):
        return self.store.publish(self.events.observe(alert or self.base), NS)

    async def test_empty_http_and_ws_are_explicit_json(self):
        for path, empty in [('/api/v1/alerts/latest', None), ('/api/v1/alerts', [])]:
            resp = await self.client.get(path)
            self.assertEqual(resp.content_type, 'application/json')
            self.assertEqual(resp.headers['Cache-Control'], 'no-store')
            self.assertEqual((await resp.json())['data'], empty)
        ws = await self.client.ws_connect('/api/v1/ws')
        snapshot = await ws.receive_json()
        self.assertEqual(snapshot['type'], 'snapshot')
        self.assertEqual(snapshot['data']['alerts'], [])
        await ws.close()

    async def test_latest_history_id_filters_schema(self):
        first = self.post()
        second = self.post(synthetic(self.base, action='update', number=1))
        resp = await self.client.get('/api/v1/alerts/latest')
        value = await resp.json()
        self.assertEqual(value['data']['id'], second.id)
        Draft202012Validator(SCHEMA, format_checker=FormatChecker()).validate(value['data'])
        resp = await self.client.get('/api/v1/alerts?service=DCR&category=MARINE&limit=1')
        self.assertEqual(len((await resp.json())['data']), 1)
        resp = await self.client.get('/api/v1/alerts/' + first.id)
        self.assertEqual((await resp.json())['data']['id'], first.id)
        self.assertEqual((await self.client.get('/api/v1/alerts/sf2-' + '0'*64)).status, 404)

    async def test_query_validation_and_json_errors(self):
        for query in ('limit=0', 'limit=101', 'limit=NaN', 'service=SLAS', 'status=training', 'x=1', 'limit=1&limit=2', 'category=bad', 'view=bad'):
            resp = await self.client.get('/api/v1/alerts?' + query)
            self.assertEqual(resp.status, 400, query)
            self.assertEqual(resp.content_type, 'application/json')
        for path in ('/missing', '/api/v1/alerts/nonsense'):
            self.assertEqual((await self.client.get(path)).content_type, 'application/json')
        self.assertEqual((await self.client.post('/api/v1/status')).status, 405)

    async def test_ws_multiple_clients_order_update_cancel_clear(self):
        clients = [await self.client.ws_connect('/api/v1/ws') for _ in range(2)]
        for ws in clients:
            await ws.receive_json()
        previous = 0
        for i, action in enumerate(('issue', 'update', 'cancel', 'clear')):
            record = self.post(synthetic(self.base, action=action, number=i))
            for ws in clients:
                message = await asyncio.wait_for(ws.receive_json(), 1)
                self.assertEqual(message['type'], 'alert')
                self.assertEqual(message['data']['action'], action)
                self.assertEqual(message['sequence'], record.sequence)
            self.assertGreater(record.sequence, previous)
            previous = record.sequence
        for ws in clients:
            await ws.close()

    async def test_repeated_satellite_semantics_do_not_push(self):
        ws = await self.client.ws_connect('/api/v1/ws')
        await ws.receive_json()
        self.post()
        await ws.receive_json()
        repeat = replace(self.base, receipt=replace(self.base.receipt, sv_id=7, timestamp_us=200))
        self.assertIsNone(self.post(repeat))
        with self.assertRaises(TimeoutError):
            await asyncio.wait_for(ws.receive_json(), .03)
        self.assertEqual(self.store.status()['alerts_cached'], 1)
        await ws.close()

    async def test_training_separated_in_http_and_ws(self):
        actual = await self.client.ws_connect('/api/v1/ws')
        training = await self.client.ws_connect('/api/v1/ws?status=test')
        await actual.receive_json(); await training.receive_json()
        self.post(synthetic(self.base, status='test', number=1))
        message = await training.receive_json()
        self.assertFalse(message['data']['is_real_disaster'])
        resp = await self.client.get('/api/v1/alerts/latest')
        self.assertIsNone((await resp.json())['data'])
        self.post(self.base)
        self.assertTrue((await actual.receive_json())['data']['is_real_disaster'])
        resp = await self.client.get('/api/v1/alerts?status=all')
        self.assertEqual(len((await resp.json())['data']), 2)
        await actual.close(); await training.close()

    async def test_snapshot_commit_race_and_http_ws_same_record(self):
        self.post()
        ws = await self.client.ws_connect('/api/v1/ws')
        snapshot = await ws.receive_json()
        second = self.post(synthetic(self.base, number=2))
        message = await ws.receive_json()
        self.assertGreater(message['sequence'], snapshot['sequence'])
        resp = await self.client.get('/api/v1/alerts/latest')
        self.assertEqual((await resp.json())['data'], message['data'])
        self.assertEqual(message['data']['id'], second.id)
        await ws.close()

    async def test_thread_commit_wakes_event_loop_immediately(self):
        ws = await self.client.ws_connect('/api/v1/ws')
        await ws.receive_json()
        await asyncio.to_thread(self.post)
        self.assertEqual((await asyncio.wait_for(ws.receive_json(), .5))['type'], 'alert')
        await ws.close()

    async def test_disconnect_is_status_and_history_not_live(self):
        self.post()
        ws = await self.client.ws_connect('/api/v1/ws')
        await ws.receive_json()
        self.store.set_receiver('disconnected')
        status = await ws.receive_json()
        self.assertEqual(status['type'], 'status')
        self.assertEqual(status['data']['data_mode'], 'historical')
        resp = await self.client.get('/api/v1/alerts/latest')
        self.assertIsNotNone((await resp.json())['data'])
        await ws.close()

    async def test_origin_policy_and_cors(self):
        resp = await self.client.get('/api/v1/alerts', headers={'Origin':'https://untrusted.invalid'})
        self.assertEqual(resp.status, 403)
        with self.assertRaises(WSServerHandshakeError):
            await self.client.ws_connect('/api/v1/ws', headers={'Origin':'https://untrusted.invalid'})
        origin = str(self.client.make_url('/')).rstrip('/')
        resp = await self.client.get('/api/v1/status', headers={'Origin':origin})
        self.assertEqual(resp.headers['Access-Control-Allow-Origin'], origin)
        self.assertEqual((await self.client.get('/api/v1/status', headers={'Origin':'http://['})).status, 403)

    async def test_ping_pong_read_only_and_cleanup(self):
        ws = await self.client.ws_connect('/api/v1/ws', autoping=False)
        await ws.receive_json()
        await ws.ping(b'check')
        self.assertEqual((await ws.receive()).type, WSMsgType.PONG)
        await ws.send_str('unsupported')
        self.assertEqual((await ws.receive()).type, WSMsgType.CLOSE)
        await ws.close()
        for _ in range(100):
            if self.store.status()['clients'] == 0:
                break
            await asyncio.sleep(.001)
        self.assertEqual(self.store.status()['clients'], 0)

    async def test_slow_client_overflow_does_not_block_http(self):
        ws = await self.client.ws_connect('/api/v1/ws')
        await ws.receive_json()
        for i in range(40):  # yield せず、送信キューを決定的に満たす
            self.post(synthetic(self.base, number=i))
        self.assertEqual((await ws.receive()).type, WSMsgType.CLOSE)
        self.assertEqual(ws.close_code, 1013)
        self.assertEqual((await self.client.get('/api/v1/alerts')).status, 200)
        await ws.close()
        reconnected = await self.client.ws_connect('/api/v1/ws')
        self.assertEqual(len((await reconnected.receive_json())['data']['alerts']), 40)
        await reconnected.close()

    async def test_real_hgraw_replay_to_http_and_ws_snapshot(self):
        # 同じ実受信バイナリを使い、合成放送を再構築しない。
        await asyncio.to_thread(receive, ReceiverConfig(replay=str(FIXTURE)), self.store, threading.Event())
        resp = await self.client.get('/api/v1/alerts/latest')
        value = await resp.json()
        self.assertEqual(value['data']['service'], 'DCR')
        ws = await self.client.ws_connect('/api/v1/ws')
        self.assertEqual((await ws.receive_json())['data']['alerts'][0], value['data'])
        await ws.close()

    async def test_close_server_closes_websockets(self):
        ws = await self.client.ws_connect('/api/v1/ws')
        await ws.receive_json()
        await self.client.server.close()
        self.assertEqual((await ws.receive()).type, WSMsgType.CLOSE)
        await ws.close()
        self.assertEqual(self.store.status()['clients'], 0)

    async def test_internal_exception_does_not_leak_paths(self):
        with patch.object(self.store, 'query', side_effect=RuntimeError('/private/file')), self.assertLogs('hoshiguard.api'):
            resp = await self.client.get('/api/v1/alerts')
        self.assertEqual(resp.status, 500)
        self.assertEqual(await resp.json(), {'error':'internal_error'})

    async def test_client_and_request_limits(self):
        other = TestClient(TestServer(create_app(AlertStore(maximum_clients=1), requests_per_second=1)))
        await other.start_server()
        try:
            ws = await other.ws_connect('/api/v1/ws')
            await ws.receive_json()
            with self.assertRaises(WSServerHandshakeError) as error:
                await other.ws_connect('/api/v1/ws')
            self.assertEqual(error.exception.status, 503)
            self.assertEqual((await other.get('/api/v1/status')).status, 429)
            await ws.close()
        finally:
            await other.close()

"""公開処理の合成した境界例と、資源上限の不変条件。"""


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
        Draft202012Validator.check_schema(SCHEMA)
        for path in (ROOT / 'docs/public-alert-real-marine.json',
                     ROOT / 'docs/public-alert-real-dcx.json'):
            validator.validate(json.loads(path.read_text()))

    def test_public_examples_match_original_hgraw_records(self):
        from hashlib import sha256
        from hoshiguard.output.raw_archive import RawArchiveReader
        cases = (
            ('marine', 'bridge_real_session', 220,
             '74b27282f1438cc8e4080f771b36e93ed8bc635cb01d65a632220dcf9575eaa9'),
            ('dcx', 'bridge_24h_session', 4608,
             'e5cb12abb3295f981bbc39557a3dbd670eabff51d9eacf341e1cbdc19cdb49ed'),
        )
        for name, fixture, offset, digest in cases:
            with self.subTest(name=name):
                source = ROOT / 'test/fixtures' / fixture / 'selected_records.hgraw'
                self.assertEqual(sha256(source.read_bytes()).hexdigest(), digest)
                record = next(item for item in RawArchiveReader(str(source))
                              if item.offset == offset)
                pipeline = ReceiverPipeline(AlertStore(replay=True))
                result = pipeline.process(record.frame, record.pc_receive_ns)
                self.assertIsNotNone(result)
                expected = json.loads((ROOT / f'docs/public-alert-real-{name}.json').read_text())
                self.assertEqual(json.loads(result.encoded), expected)


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

"""実 loopback でのプロセス起動、signal 終了、異常終了と再生復旧の検証。"""


class ApiProcessTests(unittest.TestCase):
    def start(self, replay=True):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            port = sock.getsockname()[1]
        source = ['--replay', str(FIXTURE)] if replay else ['--port', '/dev/hoshiguard-synthetic-missing-device']
        proc = subprocess.Popen([sys.executable, '-m', 'hoshiguard', *source,
                                 '--api', '--api-port', str(port)], cwd=ROOT,
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        def cleanup():
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill(); proc.wait(timeout=2)
            proc.stderr.close()
        self.addCleanup(cleanup)
        base = f'http://127.0.0.1:{port}'
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                self.fail('API exited: ' + proc.stderr.read().decode())
            try:
                status = self.get(base + '/api/v1/status')
                if status['receiver'] in ('replay_complete', 'disconnected'):
                    return proc, base, status
            except (OSError, urllib.error.URLError):
                pass
            time.sleep(.02)
        self.fail('API did not become ready')

    @staticmethod
    def get(url):
        with urllib.request.urlopen(url, timeout=.5) as response:
            return json.load(response)

    def test_sigterm_shutdown_and_replay_restart(self):
        first, base, status1 = self.start()
        before = self.get(base + '/api/v1/alerts/latest')
        self.assertEqual(before['data']['provenance']['input'], 'replay')
        first.terminate()
        self.assertEqual(first.wait(timeout=5), 0)
        second, base2, status2 = self.start()
        after = self.get(base2 + '/api/v1/alerts/latest')
        self.assertEqual(before['data'], after['data'])
        self.assertNotEqual(status1['process_id'], status2['process_id'])
        self.assertEqual(status2['data_mode'], 'historical')

    def test_abrupt_exit_restart_never_presents_previous_cache_as_live(self):
        first, base, _ = self.start()
        self.assertIsNotNone(self.get(base + '/api/v1/alerts/latest')['data'])
        first.kill()
        first.wait(timeout=5)
        second, base2, status = self.start(replay=False)
        self.assertEqual(status['receiver'], 'disconnected')
        self.assertEqual(status['data_mode'], 'historical')
        self.assertIsNone(self.get(base2 + '/api/v1/alerts/latest')['data'])
        second.terminate()
        self.assertEqual(second.wait(timeout=5), 0)

if __name__ == "__main__":
    unittest.main()
