#!/usr/bin/env python3
"""
Bridge: deepstream-app's per-frame target JSON datagrams (UDS SOCK_DGRAM)
        →  one or more remote WebSocket servers' `publish` action.

Reads each datagram from $TARGETS_UDS and, every TM_DELTA_CADENCE_SEC, fans
the latest one out to every active peer as:

    {"action":"publish","payload":{"event":"frame_detection","data":<json>}}

Each remote `wss/server.py` re-broadcasts to its own browser clients.

Env:
  TARGETS_UDS           Path of the UDS to bind. Default /run/swl/targets.sock
                        (use the host-side path when running on the host).
  REMOTE_WS             Peer source. One of:
                          "auto" (default) — discover via Beam's
                            GET $BEAM_API/api/contacts/ips and refresh every
                            DISCOVERY_PERIOD_SEC. Try-once-per-cycle: a peer
                            that doesn't accept the WS connect is dropped
                            until the next discovery cycle re-includes it.
                          "ws://a:8000,ws://b:8000" — explicit static list.
                            Senders use exponential reconnect (back-compat).
  BEAM_API              Beam REST root. Default http://localhost:9091.
  DISCOVERY_PERIOD_SEC  Discovery refresh cadence. Default 30.
  WS_PORT               Port to assume on each discovered peer. Default 8000.
  EVENT_NAME            Event name to publish under. Default "frame_detection"
  TM_DELTA_CADENCE_SEC  Publish cadence in seconds — same env var the C++
                        target-manager flusher uses, so browser overlay
                        updates land in lockstep with mesh pushes. Default 5.

Cadence semantics (latest-wins): every UDS datagram overwrites a single slot;
a ticker fires every TM_DELTA_CADENCE_SEC seconds, snapshots the slot, clears
it, and pushes the snapshot into one size-1 queue per active peer (latest-wins
overwrite). A wedged peer's queue stays at size 1 with the latest msg; it
does not block other peers.

Wire trimming: only the fields the browser overlay needs are forwarded —
top-level frame/pts_ns/ts_us/src, and per-target id+bbox. label, conf,
foot_px, class_id, etc. are dropped (class_id is recoverable from the
high bits of id anyway).
"""

import asyncio
import json
import logging
import os
import socket
import urllib.error
import urllib.request

import websockets

UDS_PATH         = os.environ.get("TARGETS_UDS", "/run/swl/targets.sock")
REMOTE_WS        = os.environ.get("REMOTE_WS", "auto")
BEAM_API         = os.environ.get("BEAM_API", "http://localhost:9091").rstrip("/")
DISCOVERY_PERIOD = max(1.0, float(os.environ.get("DISCOVERY_PERIOD_SEC", "30")))
WS_PORT          = int(os.environ.get("WS_PORT", "8000"))
EVENT_NAME       = os.environ.get("EVENT_NAME", "frame_detection")
# Same env var the C++ target-manager flusher reads (default 5 there too),
# so WS publish cadence matches mesh push cadence.
PUBLISH_PERIOD   = max(0.05, float(os.environ.get("TM_DELTA_CADENCE_SEC", "5")))


def _parse_endpoints(raw: str) -> list[str]:
    return [u.strip() for u in raw.replace(",", " ").split() if u.strip()]

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
)
log = logging.getLogger("uds-ws-bridge")


async def _bind_uds() -> socket.socket:
    try:
        os.unlink(UDS_PATH)
    except FileNotFoundError:
        pass
    parent = os.path.dirname(UDS_PATH)
    if parent and not os.path.isdir(parent):
        os.makedirs(parent, exist_ok=True)
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    sock.bind(UDS_PATH)
    os.chmod(UDS_PATH, 0o666)
    sock.setblocking(False)
    log.info("UDS bound at %s", UDS_PATH)
    return sock


class _LatestSlot:
    """Latest-wins buffer keyed by `src` (camera index). Each new datagram
    from a given source overwrites the previous one; on tick, one message per
    source is emitted so both cameras reach the client every cadence interval.
    """
    def __init__(self) -> None:
        self.by_src: dict[int, bytes] = {}
        self.received = 0

    def put(self, raw: bytes, src: int) -> None:
        self.by_src[src] = raw
        self.received += 1

    def drain(self) -> list[bytes]:
        items = list(self.by_src.values())
        self.by_src.clear()
        return items


class _UdsProto(asyncio.DatagramProtocol):
    def __init__(self, slot: _LatestSlot):
        self.slot = slot

    def datagram_received(self, data, addr):
        try:
            src = json.loads(data).get("src", 0)
        except Exception:
            src = 0
        self.slot.put(data, src)


def _slim(data: dict) -> dict:
    """Trim a deepstream datagram to the minimum the browser overlay needs:
    `pts_ns`, `src` (camera index), and per-target `id` + `bbox`.
    """
    out_targets = []
    for t in data.get("targets", []) or []:
        if not isinstance(t, dict):
            continue
        slim = {}
        if "id"   in t: slim["id"]   = t["id"]
        if "bbox" in t: slim["bbox"] = t["bbox"]
        if slim:
            out_targets.append(slim)
    out = {"targets": out_targets}
    if "pts_ns" in data:
        out["pts_ns"] = data["pts_ns"]
    if "src" in data:
        out["src"] = data["src"]
    return out


async def _ticker(slot: _LatestSlot, peers: dict[str, "_PeerState"]) -> None:
    while True:
        await asyncio.sleep(PUBLISH_PERIOD)
        raws = slot.drain()
        if not raws:
            continue
        msgs = []
        for raw in raws:
            try:
                data = json.loads(raw)
            except (json.JSONDecodeError, UnicodeDecodeError) as exc:
                log.debug("bad UDS payload (%d bytes): %s", len(raw), exc)
                continue
            msgs.append(json.dumps({
                "action":  "publish",
                "payload": {"event": EVENT_NAME, "data": _slim(data)},
            }))
        if not msgs:
            continue
        # Reap pumps whose task ended (try-once died, or static pump cancelled)
        # so the ticker stops publishing into orphaned queues.
        for url, state in list(peers.items()):
            if state.task.done():
                peers.pop(url, None)
                continue
            q = state.queue
            for msg in msgs:
                if q.full():
                    try:
                        q.get_nowait()
                    except asyncio.QueueEmpty:
                        pass
                q.put_nowait(msg)


class _PeerState:
    __slots__ = ("queue", "task")
    def __init__(self, queue: asyncio.Queue, task: asyncio.Task):
        self.queue = queue
        self.task = task


async def _ws_pump_once(url: str, queue: asyncio.Queue) -> None:
    """One connect attempt. If it fails, return — the discovery loop will
    decide whether to respawn next cycle. If it succeeds, stream until the
    socket drops, then return (same rule)."""
    sent = 0
    try:
        log.info("[%s] connecting (try-once)", url)
        async with websockets.connect(url, open_timeout=5, ping_interval=20) as ws:
            log.info("[%s] connected → forwarding %s every %.2fs",
                     url, EVENT_NAME, PUBLISH_PERIOD)
            while True:
                msg = await queue.get()
                await ws.send(msg)
                sent += 1
                if sent % 20 == 0:
                    log.info("[%s] forwarded %d publishes", url, sent)
    except (OSError, asyncio.TimeoutError, websockets.exceptions.WebSocketException) as exc:
        log.info("[%s] not reachable: %s — will retry on next discovery", url, exc)


async def _ws_pump_static(url: str, queue: asyncio.Queue) -> None:
    """Static-list mode: exponential-backoff reconnect, never gives up."""
    backoff = 1.0
    sent = 0
    while True:
        try:
            log.info("[%s] connecting", url)
            async with websockets.connect(url, open_timeout=5, ping_interval=20) as ws:
                log.info("[%s] connected → forwarding %s every %.2fs",
                         url, EVENT_NAME, PUBLISH_PERIOD)
                backoff = 1.0
                while True:
                    msg = await queue.get()
                    await ws.send(msg)
                    sent += 1
                    if sent % 20 == 0:
                        log.info("[%s] forwarded %d publishes", url, sent)
        except (OSError, asyncio.TimeoutError, websockets.exceptions.WebSocketException) as exc:
            log.warning("[%s] disconnected: %s — reconnecting in %.1fs", url, exc, backoff)
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, 30.0)


def _fetch_peers_blocking() -> tuple[str | None, list[str]]:
    """Blocking GET BEAM_API/api/contacts/ips → (selfIp, [virtualIp,...])."""
    req = urllib.request.Request(f"{BEAM_API}/api/contacts/ips")
    with urllib.request.urlopen(req, timeout=5) as resp:
        payload = json.load(resp)
    self_ip = payload.get("selfIp")
    ips = [c.get("virtualIp") for c in payload.get("contacts") or [] if c.get("virtualIp")]
    return self_ip, ips


async def _discovery_loop(peers: dict[str, _PeerState]) -> None:
    loop = asyncio.get_running_loop()
    while True:
        try:
            self_ip, ips = await loop.run_in_executor(None, _fetch_peers_blocking)
        except (urllib.error.URLError, OSError, ValueError, json.JSONDecodeError) as exc:
            log.warning("discovery: fetch failed (%s) — keeping current peer set", exc)
            await asyncio.sleep(DISCOVERY_PERIOD)
            continue

        wanted = {f"ws://{ip}:{WS_PORT}" for ip in ips if ip and ip != self_ip}
        current = set(peers.keys())

        # Spawn for newly-discovered peers. Existing connected peers are left
        # alone — their pump is still running.
        for url in wanted - current:
            q: asyncio.Queue = asyncio.Queue(maxsize=1)
            t = asyncio.create_task(_ws_pump_once(url, q), name=f"pump:{url}")
            peers[url] = _PeerState(q, t)

        # Drop peers that vanished from the contacts list.
        for url in current - wanted:
            state = peers.pop(url, None)
            if state is not None:
                state.task.cancel()
                log.info("[%s] no longer in contacts — cancelled", url)

        log.info("discovery: self=%s active=%d/%d (wanted=%d)",
                 self_ip, sum(1 for s in peers.values() if not s.task.done()),
                 len(peers), len(wanted))
        await asyncio.sleep(DISCOVERY_PERIOD)


async def main() -> None:
    raw = REMOTE_WS.strip()
    use_dynamic = (raw.lower() == "auto" or raw == "")
    static_endpoints = [] if use_dynamic else _parse_endpoints(raw)
    if not use_dynamic and not static_endpoints:
        raise SystemExit("REMOTE_WS resolved to zero endpoints")

    slot = _LatestSlot()
    sock = await _bind_uds()
    loop = asyncio.get_running_loop()
    transport, _ = await loop.create_datagram_endpoint(
        lambda: _UdsProto(slot), sock=sock,
    )

    peers: dict[str, _PeerState] = {}
    tasks: list[asyncio.Task] = [asyncio.create_task(_ticker(slot, peers), name="ticker")]

    if use_dynamic:
        log.info("dynamic discovery: %s every %.0fs (port %d)",
                 BEAM_API, DISCOVERY_PERIOD, WS_PORT)
        tasks.append(asyncio.create_task(_discovery_loop(peers), name="discovery"))
    else:
        log.info("static fan-out to %d peer(s): %s", len(static_endpoints), static_endpoints)
        for url in static_endpoints:
            q: asyncio.Queue = asyncio.Queue(maxsize=1)
            t = asyncio.create_task(_ws_pump_static(url, q), name=f"pump:{url}")
            peers[url] = _PeerState(q, t)

    try:
        await asyncio.gather(*tasks)
    finally:
        for t in tasks:
            t.cancel()
        for state in peers.values():
            state.task.cancel()
        transport.close()
        sock.close()
        try:
            os.unlink(UDS_PATH)
        except FileNotFoundError:
            pass


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
