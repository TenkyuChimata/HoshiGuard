#!/usr/bin/env python3
"""Reproducible decode/state and synthetic bounded-memory comparison.

--baseline selects the pre-0.3 diagnostic decode/state path; --package-root
allows loading an exported old checkout in a fresh interpreter. No hardware,
archive writes, fsync or real-time duration is simulated as actual reception.
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
    parser.add_argument('--package-root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--baseline', action='store_true')
    parser.add_argument('--frames', type=int, default=2000)
    parser.add_argument('--unique', type=int, default=8000)
    parser.add_argument('--network', action='store_true')
    args = parser.parse_args()
    root = args.package_root.resolve()
    sys.path.insert(0, str(root / 'python'))
    from hoshiguard.bridge_protocol import FrameType
    from hoshiguard.bridge_frames import parse_payload
    from hoshiguard.events import EventProcessor
    from hoshiguard.l1s import compare_claim
    from hoshiguard.models import json_safe
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
        raise RuntimeError('missing real DCR base')

    if args.baseline:
        events = EventProcessor()
        def process(frame, ns):
            claim = parse_payload(frame)
            checked = compare_claim(claim)
            if checked.mismatch:
                raise RuntimeError('invalid benchmark fixture')
            alert = make_alert(frame, claim, checked.verified)
            result = events.observe(alert)
            if result.disposition != 'repeated':
                json.dumps(json_safe(alert), ensure_ascii=False, separators=(',', ':'))
    else:
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
    if args.baseline:
        del events
    else:
        del pipeline
    gc.collect()
    tracemalloc.start()
    if args.baseline:
        events, store = EventProcessor(), None
    else:
        events = EventProcessor(capacity=4096, maximum_frame_fingerprints=0, track_evidence=False)
        store = AlertStore(replay=True)
    for i in range(args.unique):
        # Synthetic semantic diversity only; these are not received broadcasts.
        fields = dict(base.payload.fields)
        fields['synthetic_benchmark_discriminator'] = i
        fingerprint = hashlib.sha256(str(i).encode()).hexdigest()
        alert = replace(base, payload=replace(base.payload, fields=tuple(sorted(fields.items()))),
                        identity=replace(base.identity, semantic_fingerprint=fingerprint),
                        receipt=replace(base.receipt, timestamp_us=i))
        result = events.observe(alert)
        if store:
            store.publish(result, records[0].pc_receive_ns)
    gc.collect()
    current, peak = tracemalloc.get_traced_memory()
    tracemalloc.stop()
    ordered = sorted(latencies)
    result = {'schema':'hoshiguard.api-benchmark.v1', 'mode':'baseline' if args.baseline else 'production',
              'python':platform.python_version(), 'platform':platform.platform(),
              'fixture':str(fixture.relative_to(root)), 'fixture_sha256':hashlib.sha256(fixture.read_bytes()).hexdigest(),
              'frames':args.frames, 'decode_state_seconds':round(elapsed, 6),
              'frames_per_second':round(args.frames / elapsed, 2),
              'latency_us':{'p50':round(statistics.median(ordered), 2), 'p95':round(ordered[int(len(ordered)*.95)], 2), 'p99':round(ordered[int(len(ordered)*.99)], 2)},
              'synthetic_unique_messages':args.unique, 'retained_python_bytes':current,
              'peak_python_bytes':peak, 'semantic_entries':len(events.semantic),
              'event_entries':len(events.events), 'public_entries':store.status()['alerts_cached'] if store else None,
              'scope':'decode/state only; excludes imports, disk, hardware and server; unique-message memory input is synthetic'}
    if args.network and not args.baseline:
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
                raise RuntimeError('HTTP benchmark request rejected')
            await response.read()
            latencies.append((time.perf_counter_ns() - start) / 1000)
        cpu, wall = time.process_time(), time.perf_counter()
        await asyncio.sleep(2)
        idle_cpu, idle_wall = time.process_time() - cpu, time.perf_counter() - wall
        ordered = sorted(latencies)
        return {'requests':100, 'latency_us':{'p50':round(statistics.median(ordered), 2),
                'p95':round(ordered[95], 2), 'p99':round(ordered[99], 2)},
                'idle_wall_seconds':round(idle_wall, 6), 'idle_cpu_seconds':round(idle_cpu, 6),
                'scope':'loopback, sequential requests; two-second idle sample, not a soak test'}
    finally:
        await client.close()


if __name__ == '__main__':
    main()
