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
"""

import asyncio
import base64
import json
import math
import random
import time
import uuid
import logging
from datetime import datetime, timezone
from typing import Any

import websockets
from websockets import ServerConnection as WebSocketServerProtocol
from aiohttp import web
import aiohttp

try:
    from proto_utils import (
        base64_to_target_dict as _proto_decode,
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

# All connected websocket clients
connected: set[WebSocketServerProtocol] = set()

# Beam identity_id → target id (for create-or-update on ingest)
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
        "id": str(uuid.uuid4()),
        "updated_date": now_timestamp(),
        "tracking_location": payload.get("tracking_location") or {},
        "state": payload.get("state", "TARGET_STATE_UNKNOWN"),
        "workspace_id": payload.get("workspace_id", ""),
    }, None


def ok(action: str, data: Any) -> str:
    return json.dumps({"action": action, "status": "success", "data": data})


def err(action: str, message: str) -> str:
    return json.dumps({"action": action, "status": "error", "error": message})


def _try_decode_proto_target(content: str) -> dict | None:
    """Try to interpret a base64 string as a serialized TargetResponse proto. Returns None on failure."""
    if not _PROTO_AVAILABLE or not content:
        return None
    try:
        return _proto_decode(content)
    except Exception as exc:
        log.debug("Message content is not a proto target: %s", exc)
        return None


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
    asyncio.create_task(send_to_beam_api(target))
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
    asyncio.create_task(send_to_beam_api(target))
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
                if existing_id and existing_id in targets:
                    target = targets[existing_id]
                    target["tracking_location"] = tracking_location
                    target["state"]             = "TARGET_STATE_ACTIVE"
                    target["updated_date"]      = now_timestamp()
                    log.info("beam_event: updated target %s for identity %s", existing_id, identity_id)
                    await broadcast("target_updated", target, exclude=ws)
                else:
                    target = {
                        "id":                str(uuid.uuid4()),
                        "updated_date":      now_timestamp(),
                        "tracking_location": tracking_location,
                        "state":             "TARGET_STATE_ACTIVE",
                        "workspace_id":      workspace_id,
                        "label":             identity_name,
                        "beam_identity_id":  identity_id,
                    }
                    targets[target["id"]] = target
                    identity_targets[identity_id] = target["id"]
                    log.info("beam_event: created target %s for identity %s", target["id"], identity_id)
                    await broadcast("target_created", target, exclude=ws)

                upserted.append(target)

            elif event_type == "Message":
                content = event.get("content", "")
                target = _try_decode_proto_target(content)
                if target is not None:
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
    "create":     handle_create,
    "get":        handle_get,
    "list":       handle_list,
    "update":     handle_update,
    "delete":     handle_delete,
    "beam_event": handle_beam_event,
    "sim_start":  handle_sim_start,
    "sim_stop":   handle_sim_stop,
}


# ---------------------------------------------------------------------------
# connection handler
# ---------------------------------------------------------------------------

async def handler(ws: WebSocketServerProtocol) -> None:
    connected.add(ws)
    log.info("client connected  (%d total)", len(connected))
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

async def main() -> None:
    http_app = web.Application()
    http_app.router.add_post("/beam", http_beam)
    await start_http(http_app)

    log.info("starting target WebSocket API on ws://%s:%d", HOST, PORT)
    async with websockets.serve(handler, HOST, PORT):
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
