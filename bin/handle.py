#!/Users/matthewroberts/development/projects/somewear/xTechHackathon/xTechHack/wss/.venv/bin/python
"""
Beam inbound-hook handler.

Configuration in beam.properties:
  inbound-hook=python ~/bin/handle.py

Beam pipes one JSON payload per line to stdin. Each line is a full Beam
inbound payload object:
  {
    "requestId": "...",
    "payloads": [{
      "identity": { "id": "...", "name": "...", ... },
      "account":  { "id": "..." },
      "events":   [{ "type": "Location"|"Message"|"Data", ... }]
    }]
  }

Each payload is forwarded to the target WSS server via the "beam_event" action.
"""

import asyncio
import json
import logging
import os
import sys

import websockets

WSS_URL = os.environ.get("TARGET_WSS_URL", "ws://localhost:8000")
LOG_FILE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "wss", "handle.log")

logging.basicConfig(
    filename=LOG_FILE,
    level=logging.DEBUG,
    format="%(asctime)s %(levelname)s %(message)s",
)
log = logging.getLogger(__name__)


async def check_connection() -> bool:
    try:
        async with websockets.connect(WSS_URL, open_timeout=5):
            log.info("connectivity check OK: %s", WSS_URL)
            return True
    except Exception as exc:
        log.error("connectivity check FAILED: %s — %s", WSS_URL, exc)
        return False


async def forward(lines: list[str]) -> None:
    log.info("connecting to %s", WSS_URL)
    try:
        async with websockets.connect(WSS_URL) as ws:
            log.info("connected")
            for raw in lines:
                raw = raw.strip()
                if not raw:
                    continue

                try:
                    event = json.loads(raw)
                except json.JSONDecodeError as exc:
                    log.error("JSON parse error: %s", exc)
                    print(f"[handle.py] JSON parse error: {exc}", file=sys.stderr)
                    continue

                msg = json.dumps({"action": "beam_event", "payload": event})
                log.debug("send >> %s", msg)
                await ws.send(msg)

                resp_raw = await ws.recv()
                log.debug("recv << %s", resp_raw)
                try:
                    resp = json.loads(resp_raw)
                except json.JSONDecodeError:
                    log.error("bad server response: %s", resp_raw)
                    print(f"[handle.py] Bad server response: {resp_raw}", file=sys.stderr)
                    continue

                if resp.get("status") == "error":
                    log.error("server error: %s", resp.get("error"))
                    print(f"[handle.py] Server error: {resp.get('error')}", file=sys.stderr)
                else:
                    upserted = resp.get("data", [])
                    for t in upserted:
                        loc = t.get("tracking_location", {})
                        lat = (loc.get("latitude",  0) / 1e7)
                        lng = (loc.get("longitude", 0) / 1e7)
                        log.info(
                            "%s target=%s label=%s lat=%.5f lng=%.5f",
                            t.get("state"), t["id"][:8], t.get("label", ""), lat, lng,
                        )
                        print(
                            f"[handle.py] {t.get('state')} target={t['id'][:8]} "
                            f"label={t.get('label', '')} lat={lat:.5f} lng={lng:.5f}"
                        )

    except OSError as exc:
        log.error("cannot connect to %s: %s", WSS_URL, exc)
        print(f"[handle.py] Cannot connect to {WSS_URL}: {exc}", file=sys.stderr)
        sys.exit(1)


def main() -> None:
    log.info("handle.py invoked, pid=%d", os.getpid())
    lines = sys.stdin.readlines()
    log.info("stdin lines received: %d", len(lines))
    for i, line in enumerate(lines):
        log.debug("stdin[%d]: %s", i, line.rstrip())
    if not lines:
        log.warning("no stdin input; exiting")
        return
    asyncio.run(_run(lines))


async def _run(lines: list[str]) -> None:
    if not await check_connection():
        sys.exit(1)
    await forward(lines)


if __name__ == "__main__":
    main()
