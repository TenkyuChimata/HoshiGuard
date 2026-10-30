"""API tests: real fixture replay, plus explicitly synthetic lifecycle variants.

No synthetic mutation here is evidence of a received satellite broadcast.
Network cases bind loopback and require a runner that permits local sockets.
"""
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
from hoshiguard.bridge_frames import L1sFrame, parse_payload
from hoshiguard.bridge_protocol import BridgeFrame, FrameType
from hoshiguard.events import EventProcessor
from hoshiguard.events.canonical import semantic_fingerprint
from hoshiguard.l1s import verify_ubx_l1s
from hoshiguard.models import json_safe
from hoshiguard.output.raw_archive import RawArchiveReader
from hoshiguard.public import PublicAlert
from hoshiguard.publisher import AlertStore
from hoshiguard.routing import make_alert
from hoshiguard.runtime import ReceiverConfig, ReceiverPipeline, receive

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / 'test/fixtures/bridge_real_session/selected_records.hgraw'
NS = 1784097827645520000
SCHEMA = json.loads((ROOT / 'docs/schema/public-alert.v1.schema.json').read_text())


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
    # Change normative payload fields explicitly, not just a cached hash.
    fields = dict(base.payload.fields)
    fields['synthetic_test_discriminator'] = number
    alert = replace(base, action=action, status=status,
                    payload=replace(base.payload, fields=tuple(sorted(fields.items()))))
    return replace(alert, identity=replace(alert.identity, semantic_fingerprint=semantic_fingerprint(alert)))


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
        store.frame_received(NS)  # heartbeat is alive, no L1S since start
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
        for i in range(40):  # no yields: deterministically fill the send queue
            self.post(synthetic(self.base, number=i))
        self.assertEqual((await ws.receive()).type, WSMsgType.CLOSE)
        self.assertEqual(ws.close_code, 1013)
        self.assertEqual((await self.client.get('/api/v1/alerts')).status, 200)
        await ws.close()
        reconnected = await self.client.ws_connect('/api/v1/ws')
        self.assertEqual(len((await reconnected.receive_json())['data']['alerts']), 40)
        await reconnected.close()

    async def test_real_hgraw_replay_to_http_and_ws_snapshot(self):
        # Same real binary fixture, not a reconstructed synthetic broadcast.
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


if __name__ == '__main__':
    unittest.main()
