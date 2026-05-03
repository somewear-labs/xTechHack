"""
WebSocket API server for Target CRUD operations.

JSON shape mirrors target_proto.proto / tracking_session_proto.proto:

Target:
  id            : str  (uuid, assigned on create)
  updated_date  : { seconds: int, nanos: int }
  tracking_location: {
      longitude        : int   (sint32, scaled 1e-7 degrees)
      latitude         : int   (sint32, scaled 1e-7 degrees)
      timestamp        : int   (fixed32, unix seconds)
      altitude         : int   (sint32, millimeters)
      speed_over_ground: int   (uint32, mm/s)
      course_over_ground: int  (uint32, millidegrees)
  }
  state       : str  (TARGET_STATE_UNKNOWN | TARGET_STATE_ACTIVE | TARGET_STATE_INACTIVE |
                       TARGET_STATE_ACQUIRED | TARGET_STATE_LOST | TARGET_STATE_NEUTRALIZED)
  workspace_id: str

--- WebSocket message protocol ---

Client → server:
  { "action": "create", "payload": <target without id> }
  { "action": "get",    "payload": { "id": "..." } }
  { "action": "list",   "payload": { "workspace_id": "..." } }   # workspace_id optional
  { "action": "update", "payload": { "id": "...", ...fields } }
  { "action": "delete", "payload": { "id": "..." } }

Server → requesting client:
  { "action": "...", "status": "success", "data": <target or list or {"id":"..."}> }
  { "action": "...", "status": "error",   "error": "<message>" }

Server → all connected clients (broadcasts):
  { "event": "target_created", "data": <target> }
  { "event": "target_updated", "data": <target> }
  { "event": "target_deleted", "data": { "id": "..." } }
  { "event": "frame_detection", "data": <per-frame detections — see below> }

--- Per-frame detection stream (frame_detection) ---

Bound to deepstream-app's write_targets_socket UDS at $TARGETS_UDS
(default /home/swl-jetson-1/swl-vision/run/targets.sock). One datagram per
inferred batch is broadcast to every connected WS client.

How to subscribe (browser):

    const ws = new WebSocket("ws://<jetson-host>:8000");
    ws.onmessage = (e) => {
        const msg = JSON.parse(e.data);
        if (msg.event !== "frame_detection") return;
        const { frame, pts_ns, ts_us, src, targets } = msg.data;
        // each target: { id, class_id, label, conf, bbox: [L, T, W, H] }
        for (const t of targets) {
            // bbox is in source-resolution pixels (1280x720 for the wyze/android stream).
            // Scale to your <video> client size and draw on a <canvas> overlay.
        }
    };

For frame-accurate sync against the live RTSP stream, use the video element's
HTMLVideoElement.requestVideoFrameCallback(callback) API and match
metadata.rtpTimestamp (or mediaTime, plus an offset) against pts_ns. Buffer a
small ring of frame_detection messages keyed by pts_ns; pop the entry whose
PTS matches the painted frame.

For dev / quick check from the shell:

    websocat ws://localhost:8000           # subscribe to everything
    # or: python3 -c "import asyncio,websockets,json; \
    #     asyncio.run((async def(): ws=await websockets.connect('ws://localhost:8000'); \
    #     while 1: print(json.loads(await ws.recv())))())"
"""

import asyncio
import base64
import base64
import json
import math
import random
import logging
import os
import socket
import time
import uuid
from datetime import datetime, timezone
from typing import Any

import websockets
from websockets import ServerConnection as WebSocketServerProtocol
from aiohttp import web

# Outbound Unix-domain SOCK_DGRAM toward target-manager — every Beam Message
# event's content (base64 of TargetResponse proto bytes) gets b64-decoded and
# pushed to this socket. target-manager's listener prints/parses raw bytes.
# Socket paths default to the container layout (/run/swl/...). When running
# this server on the Jetson host (outside the container) point these env vars
# at the host-side bind-mount, e.g.
#   TM_INBOUND_SOCKET=$PWD/swl-vision/run/target-manager.sock
#   TARGETS_UDS=$PWD/swl-vision/run/targets.sock
TM_INBOUND_SOCKET = os.environ.get("TM_INBOUND_SOCKET", "/run/swl/target-manager.sock")
_tm_sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)

# Per-frame detections UDS — deepstream-app's write_targets_socket sendto's
# here, one JSON datagram per frame. The bind is opt-in: only happens when
# TARGETS_UDS env is set (e.g. on the sender Jetson). On the receiver Mac
# leave it unset and only the CRUD/Beam paths run.
TARGETS_UDS = os.environ.get("TARGETS_UDS", "")


def _forward_to_tm(raw_b64: str) -> None:
    """Decode + sendto target-manager. Tolerates the no-listener case."""
    if not raw_b64 or not TM_INBOUND_SOCKET:
        return
    try:
        raw = base64.b64decode(raw_b64)
        _tm_sock.sendto(raw, TM_INBOUND_SOCKET)
    except (FileNotFoundError, ConnectionRefusedError, OSError) as exc:
        logging.getLogger(__name__).debug("forward-to-tm dropped: %s", exc)
    except Exception as exc:  # base64 decode error or unexpected
        logging.getLogger(__name__).warning("forward-to-tm error: %s", exc)

try:
    from proto_utils import (
        base64_to_target_dict as _proto_decode,
        base64_to_target_dicts as _proto_decode_list,
        target_dict_to_bytestring as _proto_encode,
    )
    _PROTO_AVAILABLE = True
except Exception as _proto_import_err:
    _PROTO_AVAILABLE = False
    _proto_decode = None
    _proto_encode = None

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger(__name__)

HOST = "0.0.0.0"
PORT = 8000
HTTP_PORT = 8080

BEAM_API_URL      = "http://localhost:9091/api/package/async"
BEAM_WORKSPACE_ID = "71556"
BEAM_CHANNELS     = ["Radio", "Cellular"]

# Outbound sim defaults (near Shack15, SF)
_SIM_LAT      = 37.7993
_SIM_LNG      = -122.3983
_SIM_RADIUS_M = 400
_SIM_COUNT    = 3
_SIM_INTERVAL = 2.0

_sim_task: asyncio.Task | None = None

TARGET_STATES = {
    "TARGET_STATE_UNKNOWN",
    "TARGET_STATE_ACTIVE",
    "TARGET_STATE_INACTIVE",
    "TARGET_STATE_ACQUIRED",
    "TARGET_STATE_LOST",
    "TARGET_STATE_NEUTRALIZED",
}

# In-memory store: { id -> target_dict }
targets: dict[str, dict] = {}

# Beam Location events land here, not in targets
assets: dict[str, dict] = {}

# All connected websocket clients
connected: set[WebSocketServerProtocol] = set()

# Beam identity_id → asset id (for create-or-update on ingest)
identity_targets: dict[str, str] = {}


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def now_timestamp() -> dict:
    t = time.time()
    return {"seconds": int(t), "nanos": int((t % 1) * 1_000_000_000)}


def parse_iso_timestamp(ts_str: str) -> int:
    if not ts_str:
        return int(time.time())
    try:
        dt = datetime.fromisoformat(ts_str.replace("Z", "+00:00"))
        return int(dt.timestamp())
    except ValueError:
        return int(time.time())


def validate_tracking_location(loc: Any) -> str | None:
    if loc is None:
        return None
    if not isinstance(loc, dict):
        return "tracking_location must be an object"
    for field in ("longitude", "latitude", "timestamp", "altitude", "speed_over_ground", "course_over_ground"):
        if field in loc and not isinstance(loc[field], int):
            return f"tracking_location.{field} must be an integer"
    return None


def validate_state(state: Any) -> str | None:
    if state is not None and state not in TARGET_STATES:
        return f"state must be one of {sorted(TARGET_STATES)}"
    return None


def build_target(payload: dict) -> tuple[dict | None, str | None]:
    err = validate_tracking_location(payload.get("tracking_location"))
    if err:
        return None, err
    err = validate_state(payload.get("state"))
    if err:
        return None, err

    return {
        "id": payload.get("id") or str(uuid.uuid4()),
        "updated_date": now_timestamp(),
        "tracking_location": payload.get("tracking_location") or {},
        "state": payload.get("state", "TARGET_STATE_UNKNOWN"),
        "workspace_id": payload.get("workspace_id", ""),
        "label": payload.get("label", ""),
    }, None


def ok(action: str, data: Any) -> str:
    return json.dumps({"action": action, "status": "success", "data": data})


def err(action: str, message: str) -> str:
    return json.dumps({"action": action, "status": "error", "error": message})


def _try_decode_proto_targets(content: str) -> list[dict]:
    """Try to interpret a base64 string as either a TargetResponseList (batched,
    new format) or a single TargetResponse (legacy). Returns a list of target
    dicts — empty if the content didn't decode to anything recognizable."""
    if not _PROTO_AVAILABLE or not content:
        return []
    try:
        targets = _proto_decode_list(content)
    except Exception as exc:
        log.warning("Message content failed proto decode: %s (b64 prefix=%s)", exc, content[:32])
        return []
    if targets:
        log.info("proto decode OK: %d target(s) [%s]",
                 len(targets),
                 ", ".join(f"id={t.get('id')} state={t.get('state')}" for t in targets[:3])
                 + (" …" if len(targets) > 3 else ""))
    return targets


async def broadcast(event: str, data: Any, exclude: WebSocketServerProtocol | None = None) -> None:
    msg = json.dumps({"event": event, "data": data})
    recipients = connected - ({exclude} if exclude else set())
    if recipients:
        await asyncio.gather(*[ws.send(msg) for ws in recipients], return_exceptions=True)


async def send_to_beam_api(target: dict) -> None:
    """Serialize target as proto, base64-encode, and POST to the Beam package API."""
    if not _PROTO_AVAILABLE:
        return
    try:
        content = base64.b64encode(_proto_encode(target)).decode()
        body = {
            "message":     {"content": content},
            "channels":    BEAM_CHANNELS,
            "workspaceId": BEAM_WORKSPACE_ID,
        }
        async with aiohttp.ClientSession() as session:
            async with session.post(BEAM_API_URL, json=body) as resp:
                log.info("beam_api: POST %s → HTTP %d (target %s)", BEAM_API_URL, resp.status, target.get("id"))
    except Exception as exc:
        log.warning("beam_api: failed to post target %s: %s", target.get("id"), exc)


# ---------------------------------------------------------------------------
# Outbound sim
# ---------------------------------------------------------------------------

def _sim_random_location() -> dict:
    lat_deg = _SIM_RADIUS_M / 111000
    lng_deg = _SIM_RADIUS_M / (111000 * math.cos(math.radians(_SIM_LAT)))
    angle   = random.random() * 2 * math.pi
    r       = math.sqrt(random.random())
    lat     = _SIM_LAT + r * lat_deg * math.cos(angle)
    lng     = _SIM_LNG + r * lng_deg * math.sin(angle)
    return {
        "longitude":          int(lng * 1e7),
        "latitude":           int(lat * 1e7),
        "timestamp":          int(time.time()),
        "altitude":           0,
        "speed_over_ground":  random.randint(0, 15000),
        "course_over_ground": random.randint(0, 359999),
    }


async def _sim_loop() -> None:
    sim_ids = [str(uuid.uuid4()) for _ in range(_SIM_COUNT)]
    try:
        for i, tid in enumerate(sim_ids):
            target = {
                "id":                tid,
                "updated_date":      now_timestamp(),
                "tracking_location": _sim_random_location(),
                "state":             "TARGET_STATE_ACTIVE",
                "workspace_id":      "sim-outbound",
                "label":             f"SIM-{tid[:4].upper()}",
            }
            targets[tid] = target
            await broadcast("target_created", target)
            asyncio.create_task(send_to_beam_api(target))
            await asyncio.sleep(0.05)

        while True:
            await asyncio.sleep(_SIM_INTERVAL)
            tid = random.choice(sim_ids)
            if tid not in targets:
                continue
            target = targets[tid]
            target["tracking_location"] = _sim_random_location()
            target["updated_date"]      = now_timestamp()
            await broadcast("target_updated", target)
            asyncio.create_task(send_to_beam_api(target))

    except asyncio.CancelledError:
        for tid in sim_ids:
            if tid in targets:
                del targets[tid]
                await broadcast("target_deleted", {"id": tid})
        raise


async def handle_publish(ws: WebSocketServerProtocol, payload: Any) -> str:
    """Re-broadcast a {event,data} envelope to every other connected client.

    Lets a remote producer (e.g. the Jetson UDS→WS bridge) push event streams
    through this server without the producer having to bind a local socket.

    Payload shape:
        { "event": "frame_detection", "data": <arbitrary JSON> }
    """
    if not isinstance(payload, dict):
        return err("publish", "payload must be {event, data}")
    event = payload.get("event")
    if not isinstance(event, str) or not event:
        return err("publish", "payload.event (string) is required")
    if "data" not in payload:
        return err("publish", "payload.data is required")
    await broadcast(event, payload["data"], exclude=ws)
    return ok("publish", {"event": event})


async def handle_sim_start(_ws: WebSocketServerProtocol, _payload: Any) -> str:
    global _sim_task
    if _sim_task and not _sim_task.done():
        return ok("sim_start", {"running": True})
    _sim_task = asyncio.create_task(_sim_loop())
    log.info("outbound sim started")
    return ok("sim_start", {"running": True})


async def handle_sim_stop(_ws: WebSocketServerProtocol, _payload: Any) -> str:
    global _sim_task
    if _sim_task and not _sim_task.done():
        _sim_task.cancel()
        try:
            await _sim_task
        except asyncio.CancelledError:
            pass
    _sim_task = None
    log.info("outbound sim stopped")
    return ok("sim_stop", {"running": False})


# ---------------------------------------------------------------------------
# action handlers
# ---------------------------------------------------------------------------

async def handle_create(ws: WebSocketServerProtocol, payload: Any) -> str:
    if not isinstance(payload, dict):
        return err("create", "payload must be an object")

    target, error = build_target(payload)
    if error:
        return err("create", error)

    targets[target["id"]] = target
    log.info("created target %s", target["id"])
    await broadcast("target_created", target, exclude=ws)
    return ok("create", target)


async def handle_get(_ws: WebSocketServerProtocol, payload: Any) -> str:
    if not isinstance(payload, dict) or "id" not in payload:
        return err("get", "payload must contain 'id'")

    target = targets.get(payload["id"])
    if target is None:
        return err("get", f"target {payload['id']} not found")
    return ok("get", target)


async def handle_list(_ws: WebSocketServerProtocol, payload: Any) -> str:
    workspace_id = None
    if isinstance(payload, dict):
        workspace_id = payload.get("workspace_id")

    result = list(targets.values())
    if workspace_id is not None:
        result = [t for t in result if t["workspace_id"] == workspace_id]
    return ok("list", result)


async def handle_list_assets(_ws: WebSocketServerProtocol, _payload: Any) -> str:
    return ok("list_assets", list(assets.values()))


async def handle_update(ws: WebSocketServerProtocol, payload: Any) -> str:
    if not isinstance(payload, dict) or "id" not in payload:
        return err("update", "payload must contain 'id'")

    target = targets.get(payload["id"])
    if target is None:
        return err("update", f"target {payload['id']} not found")

    if "tracking_location" in payload:
        error = validate_tracking_location(payload["tracking_location"])
        if error:
            return err("update", error)
        target["tracking_location"] = payload["tracking_location"]

    if "state" in payload:
        error = validate_state(payload["state"])
        if error:
            return err("update", error)
        target["state"] = payload["state"]

    if "workspace_id" in payload:
        target["workspace_id"] = payload["workspace_id"]

    target["updated_date"] = now_timestamp()
    log.info("updated target %s", target["id"])
    await broadcast("target_updated", target, exclude=ws)
    return ok("update", target)


async def handle_delete(ws: WebSocketServerProtocol, payload: Any) -> str:
    if not isinstance(payload, dict) or "id" not in payload:
        return err("delete", "payload must contain 'id'")

    target_id = payload["id"]
    if target_id not in targets:
        return err("delete", f"target {target_id} not found")

    del targets[target_id]
    log.info("deleted target %s", target_id)
    await broadcast("target_deleted", {"id": target_id}, exclude=ws)
    return ok("delete", {"id": target_id})


async def handle_beam_event(ws: WebSocketServerProtocol, payload: Any) -> str:
    """
    Ingest a Beam inbound payload and create/update targets from Location events.
    Also broadcasts raw Message and Data events to all clients.

    Expected payload shape (Beam inbound format):
      {
        "requestId": "...",
        "payloads": [{
          "identity": {"id": "...", "name": "...", "type": "...", "email": "..."},
          "account":  {"id": "..."},
          "events":   [{"type": "Location"|"Message"|"Data", ...}]
        }]
      }
    """
    if not isinstance(payload, dict):
        return err("beam_event", "payload must be the Beam inbound JSON object")

    upserted: list[dict] = []

    for entry in payload.get("payloads", []):
        identity     = entry.get("identity", {})
        account      = entry.get("account", {})
        workspace_id = account.get("id", "")
        # Fall back to account.id when identity is absent (Beam omits it for device-only payloads)
        identity_id   = identity.get("id", "") or workspace_id
        identity_name = identity.get("name", "") or identity_id

        for event in entry.get("events", []):
            event_type = event.get("type")

            if event_type == "Location":
                try:
                    lat = float(event["latitude"])
                    lng = float(event["longitude"])
                except (KeyError, ValueError) as exc:
                    log.warning("beam_event: bad location data: %s", exc)
                    continue

                tracking_location = {
                    "longitude":         int(lng * 1e7),
                    "latitude":          int(lat * 1e7),
                    "timestamp":         parse_iso_timestamp(event.get("timestamp", "")),
                    "altitude":          0,
                    "speed_over_ground": 0,
                    "course_over_ground": 0,
                }

                existing_id = identity_targets.get(identity_id)
                if existing_id and existing_id in assets:
                    asset = assets[existing_id]
                    asset["tracking_location"] = tracking_location
                    asset["updated_date"]      = now_timestamp()
                    log.info("beam_event: updated asset %s for identity %s", existing_id, identity_id)
                    await broadcast("asset_updated", asset, exclude=ws)
                    target = asset
                else:
                    target = {
                        "id":                str(uuid.uuid4()),
                        "updated_date":      now_timestamp(),
                        "tracking_location": tracking_location,
                        "workspace_id":      workspace_id,
                        "label":             identity_name,
                        "beam_identity_id":  identity_id,
                    }
                    assets[target["id"]] = target
                    identity_targets[identity_id] = target["id"]
                    log.info("beam_event: created asset %s for identity %s", target["id"], identity_id)
                    await broadcast("asset_created", target, exclude=ws)

                upserted.append(target)

            elif event_type == "Message":
                content = event.get("content", "")
                _forward_to_tm(content)
                decoded_targets = _try_decode_proto_targets(content)
                if decoded_targets:
                    for target in decoded_targets:
                        target_id = target["id"]
                        is_new = target_id not in targets
                        target["updated_date"] = now_timestamp()
                        target["label"] = identity_name
                        target["beam_identity_id"] = identity_id
                        targets[target_id] = target
                        event_name = "target_created" if is_new else "target_updated"
                        log.info("beam_event: decoded proto target %s from Message (%s)", target_id, event_name)
                        await broadcast(event_name, target, exclude=ws)
                        upserted.append(target)
                else:
                    log.info("beam_event: Message not a TargetResponse{,List} proto, broadcasting raw (identity=%s content_prefix=%s)", identity_id, content[:32])
                    await broadcast("beam_message", {
                        "identity":   identity,
                        "account_id": workspace_id,
                        "content":    content,
                        "timestamp":  event.get("timestamp", ""),
                    })

            elif event_type == "Data":
                await broadcast("beam_data", {
                    "identity": identity,
                    "payload":  event.get("payload", ""),
                    "timestamp": event.get("timestamp", ""),
                })

    return ok("beam_event", upserted)


HANDLERS = {
    "create":      handle_create,
    "get":         handle_get,
    "list":        handle_list,
    "list_assets": handle_list_assets,
    "update":      handle_update,
    "delete":      handle_delete,
    "beam_event":  handle_beam_event,
    "sim_start":   handle_sim_start,
    "sim_stop":    handle_sim_stop,
}


# ---------------------------------------------------------------------------
# connection handler
# ---------------------------------------------------------------------------

async def handler(ws: WebSocketServerProtocol) -> None:
    global _sim_task
    connected.add(ws)
    log.info("client connected  (%d total)", len(connected))
    # Tell the client whether the outbound sim is currently running
    await ws.send(json.dumps({
        "event": "sim_state",
        "data":  {"running": _sim_task is not None and not _sim_task.done()},
    }))
    try:
        async for raw in ws:
            log.info("recv << %s", raw)
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                await ws.send(err("unknown", "invalid JSON"))
                continue

            action = msg.get("action")
            payload = msg.get("payload")

            fn = HANDLERS.get(action)
            if fn is None:
                await ws.send(err(action or "unknown", f"unknown action '{action}'"))
                continue

            response = await fn(ws, payload)
            await ws.send(response)

    except websockets.exceptions.ConnectionClosedOK:
        pass
    except websockets.exceptions.ConnectionClosedError as e:
        log.warning("client disconnected with error: %s", e)
    finally:
        connected.discard(ws)
        log.info("client disconnected (%d total)", len(connected))
        # Stop the sim when the last client leaves — no point running with nobody watching
        if not connected:
            if _sim_task and not _sim_task.done():
                log.info("all clients disconnected — stopping outbound sim")
                _sim_task.cancel()
                try:
                    await _sim_task
                except asyncio.CancelledError:
                    pass
            _sim_task = None


# ---------------------------------------------------------------------------
# HTTP webhook
# ---------------------------------------------------------------------------

async def http_beam(request: web.Request) -> web.Response:
    peer = request.remote
    log.info("webhook: POST /beam from %s", peer)

    body = await request.read()
    log.info("webhook: raw body (%d bytes): %s", len(body), body.decode(errors="replace"))

    try:
        payload = json.loads(body)
    except json.JSONDecodeError as exc:
        log.warning("webhook: invalid JSON from %s — %s | body: %s", peer, exc, body.decode(errors="replace"))
        return web.Response(status=400, text=f"invalid JSON: {exc}")

    request_id = payload.get("requestId", "<none>")
    n_payloads = len(payload.get("payloads", []))
    log.info("webhook: requestId=%s payloads=%d", request_id, n_payloads)

    for i, entry in enumerate(payload.get("payloads", [])):
        identity = entry.get("identity", {})
        events = entry.get("events", [])
        log.info(
            "webhook: payload[%d] identity=%s(%s) events=%d",
            i, identity.get("name", ""), identity.get("id", ""), len(events),
        )
        for j, event in enumerate(events):
            log.info("webhook: payload[%d].event[%d] type=%s data=%s", i, j, event.get("type"), json.dumps(event))

    try:
        # Pass None as ws so broadcast excludes nobody (all clients receive events)
        result_json = await handle_beam_event(None, payload)
    except Exception as exc:
        log.exception("webhook: unhandled error processing requestId=%s — %s", request_id, exc)
        return web.Response(status=500, text="internal server error")

    result = json.loads(result_json)
    if result.get("status") == "error":
        log.error("webhook: beam_event error for requestId=%s — %s", request_id, result.get("error"))
    else:
        log.info("webhook: requestId=%s processed OK upserted=%d", request_id, len(result.get("data", [])))
    log.info("webhook: response >> %s", result_json)

    return web.Response(content_type="application/json", text=result_json)


async def start_http(app: web.Application) -> web.AppRunner:
    runner = web.AppRunner(app)
    await runner.setup()
    await web.TCPSite(runner, HOST, HTTP_PORT).start()
    log.info("HTTP webhook listening on http://%s:%d/beam", HOST, HTTP_PORT)
    return runner


# ---------------------------------------------------------------------------
# entry point
# ---------------------------------------------------------------------------

async def _start_targets_uds_listener():
    """Bind the per-frame detections UDS and fan out as `frame_detection` events.

    Only fires when TARGETS_UDS env is set. On any client other than the sender
    Jetson, leave it unset — there's no UDS to bind to and we don't want
    ENOENT on startup.
    """
    if not TARGETS_UDS:
        log.info("TARGETS_UDS not set — skipping per-frame UDS listener")
        return None

    try:
        os.unlink(TARGETS_UDS)
    except FileNotFoundError:
        pass

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        sock.bind(TARGETS_UDS)
    except OSError as exc:
        log.warning("targets UDS bind(%s) failed: %s — skipping listener", TARGETS_UDS, exc)
        sock.close()
        return None
    os.chmod(TARGETS_UDS, 0o666)
    sock.setblocking(False)

    class _UdsProto(asyncio.DatagramProtocol):
        def datagram_received(self, data, addr):
            try:
                payload = json.loads(data)
            except (json.JSONDecodeError, UnicodeDecodeError) as exc:
                log.debug("targets UDS bad payload: %s", exc)
                return
            asyncio.create_task(broadcast("frame_detection", payload))

    loop = asyncio.get_running_loop()
    transport, _ = await loop.create_datagram_endpoint(_UdsProto, sock=sock)
    log.info("per-frame UDS listener bound at %s", TARGETS_UDS)
    return transport


async def main() -> None:
    http_app = web.Application()
    http_app.router.add_post("/beam", http_beam)
    await start_http(http_app)

    targets_transport = await _start_targets_uds_listener()

    log.info("starting target WebSocket API on ws://%s:%d", HOST, PORT)
    try:
        async with websockets.serve(handler, HOST, PORT):
            await asyncio.Future()
    finally:
        if targets_transport is not None:
            targets_transport.close()
            try:
                os.unlink(TARGETS_UDS)
            except FileNotFoundError:
                pass


if __name__ == "__main__":
    asyncio.run(main())
