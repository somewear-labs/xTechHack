#!/usr/bin/env python3
"""
Beam inbound-hook for the **sender Jetson**.

Configuration in beam.properties (on the Jetson):
    inbound-hook=python3 /home/swl-jetson-1/xTechHack/bin/handle_jetson.py

Beam pipes one JSON payload per line to stdin. We extract every Message-event
content (base64 of a TargetUpdate or TargetResponse proto), decode, and
sendto the target-manager UDS where target-manager's inbound_loop will parse
it and apply the state transition.

This is the radio-driven counterpart to wss/inject.py — same wire format on
the UDS, but driven by a real Beam delivery instead of a mimic.

Env:
  TM_INBOUND_SOCKET   UDS path (default /home/swl-jetson-1/swl-vision/run/target-manager.sock)
  HANDLE_JETSON_LOG   Log file path (default /tmp/handle_jetson.log)
"""
import base64
import json
import logging
import os
import socket
import sys

UDS_PATH = os.environ.get(
    "TM_INBOUND_SOCKET",
    "/home/swl-jetson-1/swl-vision/run/target-manager.sock",
)
LOG_FILE = os.environ.get("HANDLE_JETSON_LOG", "/tmp/handle_jetson.log")

logging.basicConfig(
    filename=LOG_FILE,
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
)
log = logging.getLogger("handle_jetson")


def forward_to_tm(content_b64: str) -> None:
    """Decode base64 content and sendto target-manager UDS."""
    if not content_b64:
        return
    try:
        proto_bytes = base64.b64decode(content_b64)
    except Exception as exc:
        log.warning("base64 decode failed (%d chars): %s", len(content_b64), exc)
        return

    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        s.sendto(proto_bytes, UDS_PATH)
        s.close()
        log.info("forwarded %d bytes to %s", len(proto_bytes), UDS_PATH)
    except (FileNotFoundError, ConnectionRefusedError) as exc:
        log.warning("UDS unavailable (%s): %s", UDS_PATH, exc)
    except OSError as exc:
        log.warning("sendto error: %s", exc)


def process_payload(payload: dict) -> int:
    """Walk a Beam inbound payload, forward every Message event. Return count."""
    n = 0
    for entry in payload.get("payloads", []):
        identity = entry.get("identity", {})
        identity_id = identity.get("id", "")
        for event in entry.get("events", []):
            if event.get("type") != "Message":
                continue
            content = event.get("content", "")
            log.info(
                "Message from identity=%s content_prefix=%s",
                identity_id, content[:32],
            )
            forward_to_tm(content)
            n += 1
    return n


def main() -> None:
    log.info("invoked, pid=%d, uds=%s", os.getpid(), UDS_PATH)
    lines = sys.stdin.readlines()
    log.info("stdin lines: %d", len(lines))
    if not lines:
        log.warning("no stdin input; exiting")
        return
    # Temporary: dump raw stdin so we can see what shape Beam is sending.
    # `forwarded 0 Message event(s)` means the events[].type discriminator
    # isn't "Message" — could be "Data" if the paired node uses package.data
    # instead of package.message. Remove once the wire is confirmed.
    log.info("raw stdin: %s", "".join(lines)[:2000])

    total = 0
    for raw in lines:
        raw = raw.strip()
        if not raw:
            continue
        try:
            payload = json.loads(raw)
        except json.JSONDecodeError as exc:
            log.error("JSON parse error: %s; raw=%s", exc, raw[:120])
            continue
        total += process_payload(payload)
    log.info("forwarded %d Message event(s)", total)


if __name__ == "__main__":
    main()
