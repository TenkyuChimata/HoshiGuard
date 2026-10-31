#!/usr/bin/env python3
"""現行の受信・公開経路の性能と、合成入力によるメモリ上限を測定する。

ハードウェア、ディスク、長時間の実受信を検証するものではない。
"""
import argparse
import asyncio
from dataclasses import replace
import gc
import hashlib
import json
from pathlib import Path
import platform
import statistics
import sys
import time
import tracemalloc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--frames', type=int, default=2000)
    parser.add_argument('--unique', type=int, default=8000)
    parser.add_argument('--network', action='store_true')
    args = parser.parse_args()
    if args.frames < 1 or args.unique < 1:
        parser.error("--frames と --unique は 1 以上で指定してください")
    root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(root / 'python'))
    from hoshiguard.bridge_protocol import FrameType
    from hoshiguard.bridge_frames import parse_payload
    from hoshiguard.events import EventProcessor
    from hoshiguard.l1s import compare_claim
    from hoshiguard.events.canonical import semantic_fingerprint
    from hoshiguard.output.raw_archive import RawArchiveReader
    from hoshiguard.routing import make_alert
    fixture = root / 'test/fixtures/bridge_24h_session/selected_records.hgraw'
    records = [r for r in RawArchiveReader(str(fixture)) if r.frame.frame_type == FrameType.L1S_FRAME]
    base = None
    for record in records:
        claim = parse_payload(record.frame)
        alert = make_alert(record.frame, claim, compare_claim(claim).verified)
        if alert.service.value == 'DCR':
            base = alert
            break
    if base is None:
        raise RuntimeError('実受信 DCR 基準データがありません')

    from hoshiguard.publisher import AlertStore
    from hoshiguard.runtime import ReceiverPipeline
    pipeline = ReceiverPipeline(AlertStore(replay=True))
    process = pipeline.process

    latencies = []
    start = time.perf_counter()
    for i in range(args.frames):
        record = records[i % len(records)]
        frame = replace(record.frame, sequence=i)
        before = time.perf_counter_ns()
        process(frame, record.pc_receive_ns)
        latencies.append((time.perf_counter_ns() - before) / 1000)
    elapsed = time.perf_counter() - start
    del process
    del pipeline
    gc.collect()
    tracemalloc.start()
    events = EventProcessor(capacity=4096, maximum_frame_fingerprints=0, track_evidence=False)
    store = AlertStore(replay=True)
    for i in range(args.unique):
        # 意味内容の多様性を合成した値であり、実受信放送ではない。
        fields = dict(base.payload.fields)
        fields['synthetic_benchmark_discriminator'] = i
        alert = replace(base, payload=replace(base.payload, fields=tuple(sorted(fields.items()))),
                        receipt=replace(base.receipt, timestamp_us=i))
        alert = replace(alert, identity=replace(alert.identity,
                                                semantic_fingerprint=semantic_fingerprint(alert)))
        result = events.observe(alert)
        store.publish(result, records[0].pc_receive_ns)
    gc.collect()
    current, peak = tracemalloc.get_traced_memory()
    tracemalloc.stop()
    ordered = sorted(latencies)
    result = {'schema':'hoshiguard.api-benchmark.v1', 'mode':'production',
              'python':platform.python_version(), 'platform':platform.platform(),
              'fixture':str(fixture.relative_to(root)), 'fixture_sha256':hashlib.sha256(fixture.read_bytes()).hexdigest(),
              'frames':args.frames, 'decode_state_seconds':round(elapsed, 6),
              'frames_per_second':round(args.frames / elapsed, 2),
              'latency_us':{'p50':round(statistics.median(ordered), 2), 'p95':round(ordered[int(len(ordered)*.95)], 2), 'p99':round(ordered[int(len(ordered)*.99)], 2)},
              'synthetic_unique_messages':args.unique, 'retained_python_bytes':current,
              'peak_python_bytes':peak, 'semantic_entries':len(events.semantic),
              'event_entries':len(events.events), 'public_entries':store.status()['alerts_cached'],
              'scope':'デコード・状態管理のみ。import、ディスク、ハードウェア、サーバー処理は除外。メモリ測定の異なる通報は合成入力'}
    if args.network:
        result['http'] = asyncio.run(network_benchmark(store))
    print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))


async def network_benchmark(store):
    from aiohttp.test_utils import TestClient, TestServer
    from hoshiguard.api import create_app
    client = TestClient(TestServer(create_app(store)))
    await client.start_server()
    latencies = []
    try:
        for _ in range(100):
            start = time.perf_counter_ns()
            response = await client.get('/api/v1/alerts/latest')
            if response.status != 200:
                raise RuntimeError('HTTP 性能測定の要求が拒否されました')
            await response.read()
            latencies.append((time.perf_counter_ns() - start) / 1000)
        cpu, wall = time.process_time(), time.perf_counter()
        await asyncio.sleep(2)
        idle_cpu, idle_wall = time.process_time() - cpu, time.perf_counter() - wall
        ordered = sorted(latencies)
        return {'requests':100, 'latency_us':{'p50':round(statistics.median(ordered), 2),
                'p95':round(ordered[95], 2), 'p99':round(ordered[99], 2)},
                'idle_wall_seconds':round(idle_wall, 6), 'idle_cpu_seconds':round(idle_cpu, 6),
                'scope':'loopback の逐次要求と 2 秒間の待機を測定。長時間試験ではない'}
    finally:
        await client.close()


if __name__ == '__main__':
    main()
