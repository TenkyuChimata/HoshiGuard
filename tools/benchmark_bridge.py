#!/usr/bin/env python3
"""v0.2.4 の実受信間隔を使う、1 時間分の決定的な Bridge 負荷測定。"""

from __future__ import annotations

import json
from pathlib import Path
import re
import statistics
import sys
import time
import tracemalloc

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

from hoshiguard.bridge_frames import parse_payload
from hoshiguard.bridge_protocol import FrameType, SequenceTracker, decode_wire, encode_frame
from hoshiguard.l1s import compare_claim
from hoshiguard.routing import make_alert
from hoshiguard.models import json_safe


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int((len(ordered) - 1) * fraction))]


def load_arrivals():
    text = (ROOT / "test/fixtures/real/real_capture_arrivals.h").read_text()
    section = text.split("kRealL1sArrivalTimestampUs[]", 1)[1].split("};", 1)[0]
    values = [int(value) for value in re.findall(r"(\d+)ULL", section)]
    base = values[0]
    pattern = [value - base for value in values]
    span = pattern[-1] + 1_000_000
    result = []
    offset = 0
    while offset < 3_600_000_000:
        result.extend(offset + value for value in pattern
                      if offset + value < 3_600_000_000)
        offset += span
    return result


def l1s_wire(sequence, ubx_hex):
    from hoshiguard.l1s import verify_ubx_l1s
    ubx = bytes.fromhex(ubx_hex); frame = verify_ubx_l1s(ubx)
    payload = bytearray(20)
    payload[4:10] = bytes((frame.sfrbx.sv_id, 5, 1, frame.sfrbx.num_words,
                           frame.pab, frame.message_type))
    payload[10:12] = (15).to_bytes(2, "little")
    payload[12:14] = len(ubx).to_bytes(2, "little")
    payload[16:19] = bytes((0,2,5)); payload.extend(ubx)
    return encode_frame(FrameType.L1S_FRAME, sequence, 0, bytes(payload))


def main() -> int:
    manifest = json.loads((ROOT / "test/fixtures/real/manifest.json").read_text())
    real = [item for item in manifest["fixtures"] if item.get("expected_l1s")]
    arrivals = load_arrivals()
    # イベント tuple: timestamp_us、優先度（0 critical/1 L1S/2 time/3 status）、byte 数
    events = []
    seq = 0
    for index, timestamp in enumerate(arrivals):
        wire = l1s_wire(seq, real[index % len(real)]["ubx_hex"]); seq += 1
        events.append((timestamp, 1, wire))
    for second in range(3600):
        events.append((second * 1_000_000, 2,
                       encode_frame(FrameType.TIME_STATUS, seq, 0, b"\0" * 16))); seq += 1
        if second % 10 == 0:
            events.append((second * 1_000_000, 3,
                           encode_frame(FrameType.STATUS, seq, 0, b"\0" * 88))); seq += 1
    events.sort(key=lambda event: (event[0], event[1]))
    queue = []
    active = None
    last = 0
    remainder = 0
    sent = []
    drops = {"l1s":0, "critical":0, "other":0}
    high = 0
    generated_l1s = len(arrivals)
    for timestamp, priority, wire in events:
        budget_num = (timestamp - last) * 11520 + remainder
        budget, remainder = divmod(budget_num, 1_000_000)
        while budget:
            if active is None:
                if not queue: break
                active = queue.pop(0)
            amount = min(budget, active[3])
            budget -= amount; active[3] -= amount
            if active[3] == 0:
                sent.append((timestamp, len(active[2]), active[1]))
                active = None
        l1s_queued = sum(item[1] == 1 for item in queue) + (active is not None and active[1] == 1)
        reserve = max(0, 4 - l1s_queued)
        can_empty = priority <= 1 or len(queue) + (active is not None) < 7 - reserve
        if len(queue) + (active is not None) < 7 and can_empty:
            queue.append([timestamp, priority, wire, len(wire)])
        else:
            replace = next((i for i in range(len(queue)-1, -1, -1)
                            if queue[i][1] > priority and queue[i][1] != 1), None)
            if replace is not None:
                drops["other"] += 1
                queue[replace] = [timestamp, priority, wire, len(wire)]
            else:
                drops["l1s" if priority == 1 else
                      "critical" if priority == 0 else "other"] += 1
        high = max(high, len(queue) + (active is not None))
        last = timestamp
    while active is not None or queue:
        if active is None: active = queue.pop(0)
        sent.append((3_600_000_000, len(active[2]), active[1])); active = None

    tracker = SequenceTracker(); gaps = 0
    decode_latencies = []
    semantic_latencies = []
    ndjson_latencies = []
    tracemalloc.start()
    for index in range(min(2000, len(arrivals))):
        wire = l1s_wire(index, real[index % len(real)]["ubx_hex"])
        start = time.perf_counter_ns(); frame = decode_wire(wire)
        claim = parse_payload(frame); comparison = compare_claim(claim)
        decode_latencies.append((time.perf_counter_ns() - start) / 1000)
        result = tracker.observe(frame.sequence); gaps += result.missing
        start = time.perf_counter_ns()
        alert = make_alert(frame, claim, comparison.verified)
        semantic_latencies.append((time.perf_counter_ns() - start) / 1000)
        start = time.perf_counter_ns()
        json.dumps(json_safe(alert), ensure_ascii=False, allow_nan=False,
                   separators=(",", ":"))
        ndjson_latencies.append((time.perf_counter_ns() - start) / 1000)
    _current, peak_memory = tracemalloc.get_traced_memory(); tracemalloc.stop()
    second_bytes = {}
    for timestamp, length, _priority in sent:
        second = timestamp // 1_000_000
        second_bytes[second] = second_bytes.get(second, 0) + length
    report = {
        "schema":"hoshiguard.bridge-performance.v1", "duration_seconds":3600,
        "arrival_source":"v0.2.4 real capture timing repeated deterministically",
        "bridge_l1s_generated":generated_l1s,
        "bridge_l1s_sent":sum(priority == 1 for _,_,priority in sent),
        "bridge_l1s_dropped":drops["l1s"], "critical_dropped":drops["critical"],
        "other_dropped":drops["other"], "sequence_gaps":gaps,
        "queue_high_watermark":high,
        "average_bytes_per_second":round(sum(x[1] for x in sent)/3600, 3),
        "peak_bytes_per_second":max(second_bytes.values()),
        "python_frames_per_second":round(len(decode_latencies) * 1_000_000 /
                                         sum(decode_latencies), 1),
        "decoder_latency_us":{"p50":round(percentile(decode_latencies,.5),2),
                              "p95":round(percentile(decode_latencies,.95),2),
                              "p99":round(percentile(decode_latencies,.99),2)},
        "semantic_latency_us":{"p50":round(percentile(semantic_latencies,.5),2),
                               "p95":round(percentile(semantic_latencies,.95),2),
                               "p99":round(percentile(semantic_latencies,.99),2)},
        "ndjson_latency_us":{"p50":round(percentile(ndjson_latencies,.5),2),
                             "p95":round(percentile(ndjson_latencies,.95),2),
                             "p99":round(percentile(ndjson_latencies,.99),2)},
        "python_peak_tracemalloc_bytes":peak_memory,
        "semantic_alert_dropped":0
    }
    output = json.dumps(report, indent=2, sort_keys=True) + "\n"
    (ROOT / "docs/performance_v025.json").write_text(output)
    print(output, end="")
    return 0 if not drops["l1s"] and not drops["critical"] and not gaps else 1


if __name__ == "__main__":
    raise SystemExit(main())
