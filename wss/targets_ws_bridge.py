#!/usr/bin/env python3
"""
Bridge: deepstream-app's per-frame target JSON datagrams (UDS SOCK_DGRAM)
        →  WebSocket fan-out for browser overlays.

Pipeline:
  deepstream-app  --sendto-->  UDS (TARGETS_UDS)  <--bind--  this bridge
                                                                  |
                            broadcast --> all connected WS clients

Each datagram is one line of JSON:
  {"frame":N, "ts_us":N, "pts_ns":N, "src":N,
   "targets":[{"id":N,"label":"...","conf":F,"bbox":[L,T,W,H],"foot_px":[U,V]}]}

Env vars:
  TARGETS_UDS       Path to bind. Default /home/swl-jetson-1/swl-vision/run/targets.sock
  TARGETS_WS_PORT   WS listen port. Default 8766
  TARGETS_WS_HOST   WS bind host.  Default 0.0.0.0
"""

import asyncio
import logging
import os
import socket

import websockets

UDS_PATH = os.environ.get("TARGETS_UDS", "/home/swl-jetson-1/swl-vision/run/targets.sock")
WS_HOST  = os.environ.get("TARGETS_WS_HOST", "0.0.0.0")
WS_PORT  = int(os.environ.get("TARGETS_WS_PORT", "8766"))

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
)
log = logging.getLogger("targets-ws")


class UdsRecvProtocol(asyncio.DatagramProtocol):
    def __init__(self, on_payload):
        self.on_payload = on_payload
        self.count = 0

    def datagram_received(self, data, addr):
        self.count += 1
        # Schedule async fan-out so we don't block this callback.
        asyncio.create_task(self.on_payload(data))


async def main() -> None:
    # Bind the UDS as a datagram listener so deepstream-app's sendto lands here.
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

    clients: set[websockets.ServerConnection] = set()

    async def broadcast(payload: bytes) -> None:
        if not clients:
            return
        # The wire is already JSON text; just decode for WS.
        try:
            msg = payload.decode("utf-8")
        except UnicodeDecodeError:
            log.warning("non-utf8 payload (%d bytes), dropping", len(payload))
            return
        # send() is fast; failures (closed sockets) are caught and culled.
        await asyncio.gather(
            *(c.send(msg) for c in list(clients)),
            return_exceptions=True,
        )

    loop = asyncio.get_running_loop()
    transport, proto = await loop.create_datagram_endpoint(
        lambda: UdsRecvProtocol(broadcast), sock=sock,
    )
    log.info("UDS bound at %s", UDS_PATH)

    async def handle_ws(ws):
        clients.add(ws)
        log.info("WS client connect from %s (clients=%d)", ws.remote_address, len(clients))
        try:
            await ws.wait_closed()
        finally:
            clients.discard(ws)
            log.info("WS client disconnect (clients=%d)", len(clients))

    async with websockets.serve(handle_ws, WS_HOST, WS_PORT):
        log.info("WS listening on ws://%s:%d", WS_HOST, WS_PORT)
        try:
            await asyncio.Future()  # run forever
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
