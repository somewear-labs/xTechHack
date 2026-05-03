#!/usr/bin/env python3
"""
Bridge: deepstream-app's per-frame target JSON datagrams (UDS SOCK_DGRAM)
        →  remote WebSocket server's `publish` action.

Reads each datagram from $TARGETS_UDS, opens a persistent WS connection to
$REMOTE_WS, and sends:

    {"action":"publish","payload":{"event":"frame_detection","data":<json>}}

per datagram. The remote `wss/server.py` re-broadcasts to all other clients.

Env:
  TARGETS_UDS           Path of the UDS to bind. Default /run/swl/targets.sock
                        (use the host-side path when running on the host).
  REMOTE_WS             Remote WebSocket URL. Default ws://100.68.81.179:8000
  EVENT_NAME            Event name to publish under. Default "frame_detection"
  TM_DELTA_CADENCE_SEC  Publish cadence in seconds — same env var the C++
                        target-manager flusher uses, so browser overlay
                        updates land in lockstep with mesh pushes. Default 5.

Cadence semantics (latest-wins): every UDS datagram overwrites a single slot;
a timer fires every TM_DELTA_CADENCE_SEC seconds and publishes whatever is in
the slot, then clears it. Datagrams that arrive between ticks are dropped.
If no datagrams arrive in a window, no publish happens that tick.

Wire trimming: only the fields the browser overlay needs are forwarded —
top-level frame/pts_ns/ts_us/src, and per-target id+bbox. label, conf,
foot_px, class_id, etc. are dropped (class_id is recoverable from the
high bits of id anyway).

Reconnect logic: if the WS drops, reconnect with backoff and resume reading
the UDS. Datagrams that arrive while disconnected are dropped (UDP semantics
on the producer side too).
"""

import asyncio
import json
import logging
import os
import socket

import websockets

UDS_PATH       = os.environ.get("TARGETS_UDS", "/run/swl/targets.sock")
REMOTE_WS      = os.environ.get("REMOTE_WS", "ws://100.68.81.179:8000")
EVENT_NAME     = os.environ.get("EVENT_NAME", "frame_detection")
# Same env var the C++ target-manager flusher reads (default 5 there too),
# so WS publish cadence matches mesh push cadence.
PUBLISH_PERIOD = max(0.05, float(os.environ.get("TM_DELTA_CADENCE_SEC", "5")))

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
    """Single-cell, latest-wins buffer. Each new datagram overwrites."""
    __slots__ = ("data", "received")
    def __init__(self) -> None:
        self.data: bytes | None = None
        self.received = 0


class _UdsProto(asyncio.DatagramProtocol):
    def __init__(self, slot: _LatestSlot):
        self.slot = slot

    def datagram_received(self, data, addr):
        self.slot.data = data
        self.slot.received += 1


def _slim(data: dict) -> dict:
    """Trim a deepstream datagram to the minimum the browser overlay needs:
    `pts_ns` (the sync key for `requestVideoFrameCallback`) and per-target
    `id` + `bbox`. Everything else — frame number, ts_us, src, class_id,
    label, conf, foot_px — is dropped. class_id is still recoverable from
    id's high bits if a consumer needs it.
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
    return out


async def _ws_pump(slot: _LatestSlot) -> None:
    backoff = 1.0
    sent = 0
    while True:
        try:
            log.info("connecting to %s", REMOTE_WS)
            async with websockets.connect(REMOTE_WS, open_timeout=5, ping_interval=20) as ws:
                log.info(
                    "connected → forwarding %s events every %.2fs",
                    EVENT_NAME, PUBLISH_PERIOD,
                )
                backoff = 1.0
                while True:
                    await asyncio.sleep(PUBLISH_PERIOD)
                    raw = slot.data
                    slot.data = None
                    if raw is None:
                        continue
                    try:
                        data = json.loads(raw)
                    except (json.JSONDecodeError, UnicodeDecodeError) as exc:
                        log.debug("bad UDS payload (%d bytes): %s", len(raw), exc)
                        continue

                    msg = json.dumps({
                        "action":  "publish",
                        "payload": {"event": EVENT_NAME, "data": _slim(data)},
                    })
                    await ws.send(msg)
                    sent += 1
                    if sent % 20 == 0:
                        log.info("forwarded %d publishes / %d datagrams received",
                                 sent, slot.received)
        except (OSError, asyncio.TimeoutError, websockets.exceptions.WebSocketException) as exc:
            log.warning("WS disconnected: %s — reconnecting in %.1fs", exc, backoff)
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, 30.0)


async def main() -> None:
    slot = _LatestSlot()

    sock = await _bind_uds()
    loop = asyncio.get_running_loop()
    transport, _ = await loop.create_datagram_endpoint(
        lambda: _UdsProto(slot), sock=sock,
    )

    try:
        await _ws_pump(slot)
    finally:
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
