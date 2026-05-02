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
import os
import sys

import websockets

WSS_URL = os.environ.get("TARGET_WSS_URL", "ws://localhost:8765")


async def forward(lines: list[str]) -> None:
    try:
        async with websockets.connect(WSS_URL) as ws:
            for raw in lines:
                raw = raw.strip()
                if not raw:
                    continue

                try:
                    event = json.loads(raw)
                except json.JSONDecodeError as exc:
                    print(f"[handle.py] JSON parse error: {exc}", file=sys.stderr)
                    continue

                await ws.send(json.dumps({"action": "beam_event", "payload": event}))

                resp_raw = await ws.recv()
                try:
                    resp = json.loads(resp_raw)
                except json.JSONDecodeError:
                    print(f"[handle.py] Bad server response: {resp_raw}", file=sys.stderr)
                    continue

                if resp.get("status") == "error":
                    print(f"[handle.py] Server error: {resp.get('error')}", file=sys.stderr)
                else:
                    upserted = resp.get("data", [])
                    for t in upserted:
                        loc = t.get("tracking_location", {})
                        lat = (loc.get("latitude",  0) / 1e7)
                        lng = (loc.get("longitude", 0) / 1e7)
                        print(
                            f"[handle.py] {t.get('state')} target={t['id'][:8]} "
                            f"label={t.get('label', '')} lat={lat:.5f} lng={lng:.5f}"
                        )

    except OSError as exc:
        print(f"[handle.py] Cannot connect to {WSS_URL}: {exc}", file=sys.stderr)
        sys.exit(1)


def main() -> None:
    lines = sys.stdin.readlines()
    if not lines:
        return
    asyncio.run(forward(lines))


if __name__ == "__main__":
    main()
