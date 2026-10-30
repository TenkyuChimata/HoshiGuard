"""本番用 CLI。証跡分析と旧形式の出力は `diagnose` で扱う。"""
from __future__ import annotations

import argparse
import asyncio
from contextlib import suppress
import logging
import os
from pathlib import Path
import signal
import sys
import threading

from .publisher import AlertStore
from .runtime import ReceiverConfig, receive


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(prog="hoshiguard", description="QZSS DCR/DCX realtime JSON receiver")
    source = result.add_mutually_exclusive_group(required=True)
    source.add_argument("--port", help="Bridge 1.0 serial device")
    source.add_argument("--replay", metavar="HGRAW", help="replay evidence; API remains available at EOF")
    result.add_argument("--baud", type=int, default=115200)
    result.add_argument("--api", action="store_true", help="serve HTTP and WebSocket JSON")
    result.add_argument("--api-host", default="127.0.0.1")
    result.add_argument("--api-port", type=int, default=8000)
    result.add_argument("--api-origin", action="append", default=[], metavar="ORIGIN",
                        help="allow a browser origin (repeatable); no wildcard")
    result.add_argument("--history-limit", type=int, default=512, help="maximum recent messages, 1..4096")
    result.add_argument("--raw-archive", metavar="HGRAW", help="optional raw evidence, rotated at 256MiB/24h")
    result.add_argument("--verbose", action="store_true")
    result.epilog = "Optional diagnostics: hoshiguard diagnose --help"
    return result


async def run(args) -> int:
    store = AlertStore(capacity=args.history_limit, replay=bool(args.replay))
    config = ReceiverConfig(port=args.port, baud=args.baud, replay=args.replay, raw_archive=args.raw_archive)
    stop = threading.Event()
    stopping = asyncio.Event()
    finished = asyncio.Event()
    failures = []
    loop = asyncio.get_running_loop()
    runner = None
    worker = None

    def request_stop():
        stop.set()
        stopping.set()

    def receiver():
        try:
            receive(config, store, stop, on_alert=None if args.api else lambda record: print(record.encoded, flush=True))
        except BaseException as exc:
            failures.append(exc)
            logging.getLogger(__name__).exception("receiver stopped unexpectedly")
        finally:
            with suppress(RuntimeError):
                loop.call_soon_threadsafe(finished.set)

    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, request_stop)
    waiters = []
    try:
        if args.api:
            from aiohttp import web
            from .api import create_app
            runner = web.AppRunner(create_app(store, allowed_origins=tuple(args.api_origin)),
                                   access_log=None, shutdown_timeout=5,
                                   max_line_size=2048, max_field_size=2048)
            await runner.setup()
            await web.TCPSite(runner, args.api_host, args.api_port).start()
            logging.getLogger(__name__).info("API listening on %s:%s", args.api_host, args.api_port)
        worker = threading.Thread(target=receiver, name="hoshiguard-receiver", daemon=True)
        worker.start()
        waiters = [asyncio.create_task(stopping.wait()), asyncio.create_task(finished.wait())]
        await asyncio.wait(waiters, return_when=asyncio.FIRST_COMPLETED)
        if args.api and args.replay and finished.is_set() and not failures and not stopping.is_set():
            await stopping.wait()
        return 1 if failures else 0
    finally:
        stop.set()
        for task in waiters:
            task.cancel()
        for task in waiters:
            with suppress(asyncio.CancelledError):
                await task
        if worker and worker.is_alive():
            try:
                await asyncio.wait_for(finished.wait(), 3)
            except asyncio.TimeoutError:
                logging.getLogger(__name__).error("receiver shutdown timed out")
        store.set_receiver("stopped" if not failures else "failed")
        if runner:
            await runner.cleanup()
        for sig in (signal.SIGINT, signal.SIGTERM):
            loop.remove_signal_handler(sig)


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] == "diagnose":
        from .diagnostics import main as diagnose
        return diagnose(argv[1:])
    argparser = parser()
    args = argparser.parse_args(argv)
    if not 1 <= args.api_port <= 65535 or not 1 <= args.history_limit <= 4096 or args.baud <= 0:
        argparser.error("invalid port, baud or history limit")
    if args.replay and args.raw_archive:
        same = Path(args.replay).resolve() == Path(args.raw_archive).resolve()
        if Path(args.replay).exists() and Path(args.raw_archive).exists():
            same = same or os.path.samefile(args.replay, args.raw_archive)
        if same:
            argparser.error("replay input and raw archive must differ")
    if any(origin == "*" or not origin.startswith(("http://", "https://")) for origin in args.api_origin):
        argparser.error("--api-origin requires an explicit http(s) origin")
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(levelname)s %(message)s")
    try:
        return asyncio.run(run(args))
    except ImportError as exc:
        logging.error("missing optional API dependency; install hoshiguard[api]: %s", exc.name)
    except (OSError, ValueError) as exc:
        logging.error("startup failed: %s", exc)
    return 1
