"""Read-only aiohttp API. All reads use the same in-memory committed state."""
from __future__ import annotations

import asyncio
from contextlib import suppress
import logging
import re
import time
from urllib.parse import urlsplit

from aiohttp import WSMsgType, WSCloseCode, web

from .publisher import AlertStore, encode

_LOG = logging.getLogger(__name__)
_HEADERS = {"Cache-Control": "no-store", "X-Content-Type-Options": "nosniff"}
_STATUSES = {"actual", "test", "unknown", "all"}


def response(value=None, *, text: str | None = None, status: int = 200) -> web.Response:
    return web.Response(text=encode(value) if text is None else text,
                        status=status, content_type="application/json", headers=_HEADERS)


def create_app(store: AlertStore, *, allowed_origins: tuple[str, ...] = (),
               send_timeout: float = 5.0, requests_per_second: float = 100.0) -> web.Application:
    sockets: set[web.WebSocketResponse] = set()
    tokens, updated = requests_per_second * 2, time.monotonic()

    async def close_ws(ws, *, code=WSCloseCode.GOING_AWAY, message=b""):
        # Bound both socket drain and close acknowledgement. aiohttp closes
        # the transport when close() is cancelled by wait_for on timeout.
        with suppress(asyncio.TimeoutError, ConnectionError, OSError):
            await asyncio.wait_for(ws.close(code=code, message=message), 2)

    def origin_allowed(request: web.Request) -> bool:
        origin = request.headers.get("Origin")
        if not origin:
            return True  # non-browser clients
        if origin in allowed_origins:
            return True
        try:
            parts = urlsplit(origin)
        except ValueError:
            return False
        return (parts.scheme == request.scheme and parts.netloc == request.host
                and not parts.path and not parts.query and not parts.fragment)

    @web.middleware
    async def guard(request, handler):
        nonlocal tokens, updated
        now = time.monotonic()
        tokens = min(requests_per_second * 2, tokens + (now - updated) * requests_per_second)
        updated = now
        if tokens < 1:
            return response({"error": "rate_limit"}, status=429)
        tokens -= 1
        if len(request.query_string) > 1024 or request.content_length not in (None, 0) or request.headers.get("Transfer-Encoding"):
            return response({"error": "request_limit"}, status=413)
        if not origin_allowed(request):
            return response({"error": "origin_not_allowed"}, status=403)
        try:
            result = await handler(request)
        except web.HTTPException as exc:
            result = response({"error": exc.reason}, status=exc.status)
        except (ValueError, KeyError):
            result = response({"error": "invalid_query"}, status=400)
        except Exception:
            _LOG.exception("API handler failed")
            result = response({"error": "internal_error"}, status=500)
        if request.headers.get("Origin") and not isinstance(result, web.WebSocketResponse):
            result.headers["Access-Control-Allow-Origin"] = request.headers["Origin"]
            result.headers["Vary"] = "Origin"
        return result

    def query(request, *, list_query: bool = True) -> dict:
        allowed = {"service", "category", "status", "limit", "offset", "view"} if list_query else {"status"}
        if set(request.query) - allowed or any(len(request.query.getall(k)) != 1 for k in request.query):
            raise ValueError("query")
        status = request.query.get("status", "actual")
        if status not in _STATUSES:
            raise ValueError("status")
        if not list_query:
            return {"status": status}
        service = request.query.get("service")
        if service is not None and service not in ("DCR", "DCX"):
            raise ValueError("service")
        category = request.query.get("category")
        if category is not None and not re.fullmatch(r"[A-Z0-9_()'.,&/+-]{1,100}", category):
            raise ValueError("category")
        limit = int(request.query.get("limit", "100"))
        if not 1 <= limit <= 100:
            raise ValueError("limit")
        offset = int(request.query.get("offset", "0"))
        if not 0 <= offset <= store.capacity:
            raise ValueError("offset")
        view = request.query.get("view", "history")
        if view not in ("history", "states"):
            raise ValueError("view")
        return {"service": service, "category": category,
                "status": None if status == "all" else status,
                "limit": limit, "states": view == "states", "offset": offset}

    def envelope(records, health, *, single: bool, offset: int = 0) -> str:
        data = records[0].encoded if single and records else "null" if single else '[' + ','.join(r.encoded for r in records) + ']'
        return ('{"schema":"hoshiguard.api.v1","process_id":' + encode(store.process_id)
                + ',"sequence":' + str(health["sequence"])
                + ',"data":' + data + ',"next_offset":' + encode(offset + len(records) if records and not single else None)
                + ',"status":' + encode(health) + '}')

    async def alerts(request):
        options = query(request)
        latest = request.path.endswith("/latest")
        if latest:
            options["limit"] = 1
        records, health = store.query(**options)
        return response(text=envelope(records, health, single=latest, offset=options["offset"]))

    async def by_id(request):
        if request.query:
            raise ValueError("query")
        identifier = request.match_info["id"]
        if not re.fullmatch(r"sf2-[0-9a-f]{64}", identifier):
            return response({"error": "not_found"}, status=404)
        records, health = store.query(identifier=identifier, limit=1)
        return response(text=envelope(records, health, single=True), status=200 if records else 404)

    async def status(request):
        if request.query:
            raise ValueError("query")
        return response(store.status())

    async def websocket(request):
        options = query(request, list_query=False)
        try:
            sub, snapshot = store.subscribe(options["status"])
        except OverflowError:
            return response({"error": "client_limit"}, status=503)
        ws = web.WebSocketResponse(timeout=2, heartbeat=30, max_msg_size=1024, compress=False)
        sender = reader = None
        try:
            await ws.prepare(request)
            sockets.add(ws)

            async def send():
                await asyncio.wait_for(ws.send_str(snapshot), send_timeout)
                while not ws.closed:
                    overflow, message = store.pop(sub)
                    if overflow:
                        await close_ws(ws, code=WSCloseCode.TRY_AGAIN_LATER,
                                       message=b"queue overflow; reconnect for snapshot")
                        return
                    if message is None:
                        await sub.wake.wait()
                    else:
                        await asyncio.wait_for(ws.send_str(message), send_timeout)

            async def receive():
                async for message in ws:
                    if message.type in (WSMsgType.TEXT, WSMsgType.BINARY):
                        await close_ws(ws, code=WSCloseCode.POLICY_VIOLATION,
                                       message=b"read-only stream")
                        return

            sender, reader = asyncio.create_task(send()), asyncio.create_task(receive())
            done, _ = await asyncio.wait((sender, reader), return_when=asyncio.FIRST_COMPLETED)
            for task in done:
                task.result()
        except (asyncio.TimeoutError, ConnectionError, OSError):
            if request.transport:
                request.transport.close()
        except Exception:
            if not ws.prepared:
                raise
            _LOG.exception("WebSocket client failed")
            await close_ws(ws, code=WSCloseCode.INTERNAL_ERROR, message=b"stream error")
        finally:
            store.unsubscribe(sub)
            sockets.discard(ws)
            tasks = tuple(task for task in (sender, reader) if task is not None)
            for task in tasks:
                task.cancel()
            # The failure has already been logged/handled above. Retrieve all
            # task results during cleanup without attempting another HTTP
            # response after the connection has upgraded to WebSocket.
            if tasks:
                await asyncio.gather(*tasks, return_exceptions=True)
            if ws.prepared:
                await close_ws(ws)
        return ws

    async def lifecycle(app):
        store.attach(asyncio.get_running_loop())

        async def health_expiry():
            while True:
                await asyncio.sleep(1)
                store.refresh_health()
        task = asyncio.create_task(health_expiry())
        try:
            yield
        finally:
            task.cancel()
            with suppress(asyncio.CancelledError):
                await task
            store.detach()

    async def shutdown(app):
        await asyncio.gather(*(close_ws(ws, code=WSCloseCode.GOING_AWAY,
                                      message=b"service stopping") for ws in tuple(sockets)))

    app = web.Application(middlewares=[guard], client_max_size=1024)
    app.cleanup_ctx.append(lifecycle)
    app.on_shutdown.append(shutdown)
    app.router.add_get("/api/v1/alerts/latest", alerts)
    app.router.add_get("/api/v1/alerts", alerts)
    app.router.add_get("/api/v1/alerts/{id}", by_id)
    app.router.add_get("/api/v1/status", status)
    app.router.add_get("/api/v1/ws", websocket)
    return app
