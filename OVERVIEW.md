# xTechHack — Project Overview

Reference for humans and agents picking up this repo. Captures the high-level
picture, where each module lives, the wire formats between them, and the active
suppression / state-transition flow as of 2026-05-03.

## High-level picture

```
  Jetson Orin (sender)                                    Receiver (Mac)
  ────────────────────────                                ─────────────────────
  swl-vision deepstream-app  ───tm_on_batch()──▶  target-manager (this repo)
       │  (RTSP out)                                     │
       │                                                 │  TargetResponse (proto)
       │                                                 │  base64 in JSON to Beam
       │                                                 ▼
       │                                          POST localhost:9091 (Beam CLI)
       │                                                 │
       │                                                 ▼
       │                                          Beam radio link  ◀──── inbound
       │                                                                  hook
       │                                                                   │
       ▼                                                                   ▼
   RTSP stream  ──────────────────────────────▶   Mac: bin/handle.py  ─▶  WSS
                                                                          │
                                                                          ▼
                                                                    web client
                                                                    (client/)
```

Two transports run in parallel:

1. **RTSP video** — emitted by the patched DeepStream app on the sender, played
   by the web client. No bbox metadata in the stream today (see *Known
   limitations*).
2. **Beam radio data channel** — `TargetResponse` proto carrying id,
   tracking location, state, and bbox. Out-of-band relative to video.

A planned third transport — a UDS-injected `TargetUpdate` *back* into
target-manager — drives state transitions (currently INACTIVE only).

## Repo layout

```
xTechHack/
├── target-manager/        C++ static lib hooked into swl-vision DeepStream
│   ├── target_manager.{h,hpp,cpp}
│   ├── geolocation.{hpp,cpp}     pixel→GPS via camera intrinsics
│   ├── CMakeLists.txt            generates proto C++ stubs at configure time
│   └── proto/                    (currently empty / cmake-managed)
├── wss/                   Python WSS infra + protobuf schema
│   ├── target_proto.proto        canonical schema (sender + receiver share)
│   ├── target_proto_pb2.py       generated; protoc 6.31.1 via grpcio-tools
│   ├── proto_utils.py
│   ├── server.py                 receiver-side WSS API (target CRUD + broadcasts)
│   ├── echo_server.py            stdlib HTTP smoke-test for outbound Beam path
│   └── inject.py                 mimic harness: send TargetUpdate over UDS
├── bin/
│   └── handle.py                 Beam inbound-hook (runs on receiver Mac)
├── client/                Web UI + RTSP→WS bridge
│   ├── server.js                 express server + RTSP WS proxy
│   ├── emit-target.js
│   └── public/
└── OVERVIEW.md            (this file)
```

`swl-vision` and the DeepStream patch live in a separate tree; only the
`tm_*` C ABI (`target_manager.h`) crosses the boundary.

## Module responsibilities

### `target-manager` (C++)

Static library linked into the DeepStream app. Three symbols cross the C ABI
boundary (`target_manager.h`):

- `tm_init(beam_url, inbound_socket_path)` — start flusher + UDS listener.
- `tm_on_batch(NvDsBatchMeta*)` — called per inferred batch; updates targets,
  enforces suppression.
- `tm_shutdown()` — join threads, cleanup.

Internally:

- `Target` (`target_manager.hpp:28`) holds packed id, last GPS, last bbox,
  last L2-normalized ReID embedding, dirty bit.
- `TargetManager::on_batch` (`target_manager.cpp` ~ line 320) walks
  `NvDsBatchMeta → frame_meta_list → obj_meta_list`, applies suppression,
  pulls `NVDS_TRACKER_OBJ_REID_META` / foot location / world location /
  visibility user metas, and updates the live `targets_` map.
- `TargetManager::flusher_loop` — background thread; every
  `TM_DELTA_CADENCE_SEC` it snapshots dirty targets, serializes each as a
  `TargetResponse`, base64-wraps it in JSON, and POSTs to the Beam CLI's
  `/api/package/async`. Each POST is detached on its own curl handle because
  Beam's send is synchronous and can hang for tens of seconds.
- `TargetManager::inbound_loop` — UDS `SOCK_DGRAM` listener bound to
  `inbound_socket_path`. Parses each datagram as `TargetUpdate`. On INACTIVE:
  captures the live ReID feature off the target, drops it from `targets_`,
  inserts the id into `inactive_ids_`, and (if a feature was captured) appends
  it to `banned_features_`.

### `wss/` (Python, receiver side)

- `target_proto.proto` — single source of truth for schema. C++ regenerates via
  cmake at build time; Python via grpcio-tools' protoc.
- `server.py` — async WSS server. CRUD over JSON; broadcasts target events.
- `echo_server.py` — stdlib HTTP server that mimics the Beam `/api/package/async`
  endpoint for outbound smoke-testing.
- `inject.py` — small CLI that builds a `TargetUpdate` and `sendto`s the UDS.
  Used to exercise the inbound path on the sender without a real radio link.

### `bin/handle.py`

Runs on the receiver Mac as a Beam inbound hook (`inbound-hook=python ~/bin/handle.py`
in `beam.properties`). Reads stdin lines from Beam, forwards each as a
`beam_event` action to the WSS server.

### `client/`

Express + WebSocket bridge. `server.js` proxies RTSP to a browser WebSocket on
`RTSP_WS_PORT`. The web UI in `public/` consumes the live RTSP stream and the
WSS target events.

## Wire formats

### `TargetResponse` (outbound, sender → Beam)

```proto
message TargetResponse {
    uint64 id = 1;                           // packed: ((class_id+1)<<32) | object_id
    Timestamp updated_date = 2;
    TrackingLocationDto tracking_location = 3;
    TargetState state = 4;
    uint64 workspace_id = 5;
    fixed64 bbox = 6;                        // packed: (left<<48)|(top<<32)|(width<<16)|height, uint16 each
}
```

Serialized, base64-wrapped, embedded in:

```json
{"message":{"content":"<base64>"},"channels":["Radio","Cellular"],"workspaceId":"71556"}
```

POSTed to `TM_BEAM_URL` (default `http://localhost:9091/api/package/async`).

### `TargetUpdate` (inbound, UDS → target-manager)

```proto
message TargetUpdate {
    uint64 id = 1;       // same packing as TargetResponse.id
    TargetState state = 4;
}
```

Tags 1 and 4 match `TargetResponse`, so a sender can ship a full
`TargetResponse` and we'll parse just id + state (proto3 ignores unknown
fields). Raw proto bytes, one datagram per update.

### Id packing

```
id = ((class_id + 1) << 32) | object_id   // class_id+1 ensures high half ≥ 1
```

Receiver recovers `class_id = (id >> 32) - 1`, `object_id = id & 0xFFFFFFFF`.
Defined in `target_manager.hpp:43-48`. Mirrored in `wss/inject.py::pack_id`.

### Bbox packing

`fixed64`, four `uint16` lanes — left, top, width, height — at source resolution
(1280×720). Pack site: `target_manager.cpp:204-210`.

## State management

Three structures, all on `TargetManager`:

| Structure          | Type                                       | Lifetime  | Source of truth for…                       |
|--------------------|--------------------------------------------|-----------|--------------------------------------------|
| `targets_`         | `unordered_map<uint64_t, shared_ptr<Target>>` | live      | active targets: bbox, GPS, ReID, dirty bit |
| `inactive_ids_`    | `unordered_set<uint64_t>`                  | terminal  | "id-suppressed forever" — fast path        |
| `banned_features_` | `vector<vector<float>>`                    | terminal  | ReID embeddings of inactive targets        |

`inactive` is **terminal** — once an id lands in `inactive_ids_`, it never comes
back. Promotion path:

- via inbound `TargetUpdate(state=INACTIVE)`, **or**
- via ReID similarity match in `on_batch` (cosine sim ≥ `TM_REID_THRESHOLD`,
  default 0.7).

### Inbound state transitions

`inbound_loop` parses each datagram as `TargetUpdate` and dispatches on
`state`:

| State                     | Behavior                                                                                                                           |
|---------------------------|------------------------------------------------------------------------------------------------------------------------------------|
| `TARGET_STATE_ACTIVE`     | Find target in `targets_`; set `active_ack=true`, `dirty=true`. Next flusher tick emits `TargetResponse.state=ACTIVE` for that id. |
| `TARGET_STATE_INACTIVE`   | Lift live ReID feature off the target; erase from `targets_`; insert id into `inactive_ids_`; bank the feature into `banned_features_`. **Terminal.** |
| anything else             | Logged and ignored.                                                                                                                |

### Suppression order in `on_batch`

For each `NvDsObjectMeta`:

1. Compute packed id.
2. **Id-based check**: lock `inactive_mu_`; if id in `inactive_ids_`, call
   `nvds_remove_obj_meta_from_frame` and `continue`. Removes the box from
   downstream OSD / encoder / RTSP viewers too — *as long as `tm_on_batch` is
   probed upstream of `nvosd`* (see Production setup notes).
3. Walk `obj_user_meta_list` to grab ReID feature ptr / foot location / world
   location / visibility.
4. **ReID check**: if a feature is present, L2-normalize it, lock
   `inactive_mu_`, scan `banned_features_` for cosine sim ≥ threshold. Match →
   insert id into `inactive_ids_` (so step 2 short-circuits next frame),
   `nvds_remove_obj_meta_from_frame`, `continue`.
5. Geolocate, update `Target` in `targets_`, stash the normalized ReID embedding
   for capture-on-suppress later.

Iterator safety: the obj-meta list is walked with `lo_next` cached *before* the
body, since `nvds_remove_obj_meta_from_frame` frees the current node.

## Build & dev

### C++

```bash
cd target-manager/build
cmake --build .
```

CMake regenerates `target_proto.pb.{h,cc}` from `wss/target_proto.proto`
automatically (`CMakeLists.txt:23`).

### Python proto

```bash
cd wss
python3 -m grpc_tools.protoc --python_out=. -I. target_proto.proto
```

System `protoc` is 3.12.4, which doesn't match the protobuf 7.34.1 runtime;
grpcio-tools ships a compatible 6.31.1.

### Inbound mimic

```bash
# Suppress a track by packed id:
python3 wss/inject.py --socket /tmp/tm-inbound.sock --id 4294967296 --state inactive

# Or by class_id + object_id (script does the packing):
python3 wss/inject.py --socket /tmp/tm-inbound.sock --class-id 0 --object-id 12 --state inactive
```

Path must match what `tm_init` was called with on the sender.

### Outbound smoke test

```bash
python3 wss/echo_server.py     # listens on :9099, prints decoded TargetResponse
TM_BEAM_URL=http://localhost:9099/api/package/async ./your-deepstream-app
```

### Env vars

| Name                    | Default                                            | Effect                                  |
|-------------------------|----------------------------------------------------|-----------------------------------------|
| `TM_DELTA_CADENCE_SEC`  | 5                                                  | Flusher cadence (seconds)               |
| `TM_BEAM_URL`           | `http://localhost:9091/api/package/async`          | Beam HTTP endpoint                      |
| `TM_WORKSPACE_ID`       | `71556`                                            | Beam workspace id                       |
| `TM_REID_THRESHOLD`     | 0.7                                                | Cosine sim cutoff for ReID match        |
| `TM_REID_MAX_BANNED`    | 256                                                | Cap on `banned_features_` size          |

## Test loops

### Outbound smoke: target-manager → echo server

Verifies the flusher is producing well-formed `TargetResponse` protos and the
Beam-shaped JSON envelope.

```bash
# terminal A — fake Beam endpoint
python3 wss/echo_server.py        # listens on :9099, decodes content
```

```bash
# terminal B — sender (swl-vision deepstream-app), pointed at the fake endpoint
TM_BEAM_URL=http://localhost:9099/api/package/async \
  ./your-deepstream-app
```

Expect: `[tm] flush: posting N target(s) to beam` from the sender, and
decoded `TargetResponse` lines from `echo_server.py` showing the expected
ids / lat-lng / packed bbox.

### Inbound state transitions: deterministic ACTIVE / INACTIVE round trip

This is the loop the project uses to validate the unix-socket inbound path
without a real radio link.

**Pre-requisites**

- Sender pipeline is running and `tm_init(beam_url, "/path/to/uds")` was
  called with a known UDS path. Look for the boot log line:

  ```
  [tm-inbound] listening on /path/to/uds (AF_UNIX SOCK_DGRAM)
  ```

- The pipeline has been running long enough that `[tm] frame=… objs=N`
  lines show captured `obj id=K class=… (…)`. Pick two ids from those logs
  — call them `ID_ACTIVE` and `ID_INACTIVE`.

**Step 1 — capture phase**

Watch the sender stdout. You should see, every frame:

```
[tm] frame=42 pad=0 objs=2
  obj id=12 class=person(0) det=0.91 trk=0.98 bbox=(640,180 96x256) [new]
  obj id=13 class=person(0) det=0.88 trk=0.97 bbox=(310,210 88x244) [new]
```

Note both ids are class 0 → packed id of obj 12 is `(0+1)<<32 | 12 = 4294967308`.
For convenience, `inject.py` does the packing if you pass `--class-id` and
`--object-id` instead of `--id`.

**Step 2 — promote one to ACTIVE**

```bash
python3 wss/inject.py --socket /path/to/uds \
                     --class-id 0 --object-id 12 \
                     --state active
```

Expected sender log:

```
[tm-inbound] id=4294967308 -> ACTIVE
```

Within `TM_DELTA_CADENCE_SEC` the next flush should include id 12 with
`state=TARGET_STATE_ACTIVE` (visible in `echo_server.py` if you're running
the outbound loop too).

**Step 3 — a moment later, suppress the other**

```bash
python3 wss/inject.py --socket /path/to/uds \
                     --class-id 0 --object-id 13 \
                     --state inactive
```

Expected sender log:

```
[tm-inbound] id=4294967309 -> INACTIVE (terminal) [reid banked]
```

(`[reid banked]` only if at least one frame for id 13 had delivered a
`NVDS_TRACKER_OBJ_REID_META`; otherwise the suffix is omitted and only the
id is on the blacklist.)

From the next frame onward, sender stdout should show:

```
  obj id=13 class=0 SUPPRESSED (inactive) -> remove from frame
```

…and the RTSP viewer should stop drawing the box for that track (subject to
the OSD-ordering caveat in *Production setup*). If the same person re-enters
under a fresh tracker id and a ReID feature was banked, expect:

```
  obj id=87 ReID match (sim=0.812 >= 0.700) -> SUPPRESS
```

### Inbound parser fuzz

Send junk to the UDS to confirm the parser drops it cleanly:

```bash
echo -n "garbage bytes" | nc -uU /path/to/uds   # or python sendto with random bytes
```

Expect: `[tm-inbound] parse failed (N bytes)` and the loop keeps running.

## Production setup (living document)

> This section is expected to evolve. Update when the topology changes —
> ports, hosts, service-management, or the swl-vision integration shape.

### Sender — Jetson Orin Nano

**Hardware notes**

- No NVENC silicon. All H.264/H.265 encoding is software (`x264enc` /
  `x265enc` / `openh264enc`). Plan accordingly when adjusting bitrate /
  resolution / frame rate budgets.

**Components running**

| Component               | Source                              | Notes                                                                            |
|-------------------------|-------------------------------------|----------------------------------------------------------------------------------|
| `swl-vision` DS app     | separate repo (patched DeepStream 7.1) | Links `target-manager` as a static lib; calls `tm_init` at start, `tm_on_batch` per inferred batch, `tm_shutdown` on exit. |
| `target-manager`        | this repo, `target-manager/`        | See *Build & dev*.                                                               |
| Beam radio CLI          | external                            | Run under sudo by the user. Listens for HTTP POST from `target-manager` and shuttles packages over the radio link. **Do not curl/CLI Beam from automation — the user owns it.** |
| RTSP server             | embedded in the swl-vision pipeline | Default URL pattern: `rtsp://<jetson-ip>:9554/ds-test` (see `client/server.js`). |

**Probe placement caveat (open question for swl-vision)**

For inactive-track suppression to actually remove the box from the encoded
RTSP stream, `tm_on_batch` must be invoked **upstream of `nvosd`** (i.e. on
a probe attached between `nvtracker` and `nvosd`, or on `nvosd`'s sink pad).
If the probe is on the encoder src pad or after, `nvds_remove_obj_meta_from_frame`
will run too late — the OSD pixels are already burnt in. Verify in the
swl-vision pipeline graph before relying on visual suppression.

**Required env vars**

See *Env vars* table above. At minimum: `TM_BEAM_URL` if Beam isn't on
`localhost:9091`.

**UDS path**

Pass an absolute path to `tm_init`'s `inbound_socket_path` argument.
Recommended: `/tmp/tm-inbound.sock` so the inject scripts default cleanly.

### Receiver — Mac (peer)

**Components running**

| Component        | Source                | Command                                        |
|------------------|-----------------------|------------------------------------------------|
| Beam inbound hook| `bin/handle.py`       | Configured via `inbound-hook=python ~/bin/handle.py` in `beam.properties`. Edit the shebang line for your venv. |
| WSS API server   | `wss/server.py`       | `python3 -m wss.server` (or however invoked).  |
| Web client + RTSP bridge | `client/server.js` | `node client/server.js`. Set `RTSP_URL` env to the sender's RTSP endpoint. |

**Network requirements**

- Mac must be able to reach `rtsp://<jetson>:9554` (used by `client/server.js`).
- Browser opens the web client on `:3000` (or `PORT` env).
- Beam radio link is the metadata path; the WSS server is the bridge from
  `handle.py` → browser.

### Known frictions to address before "v1"

- ID format mismatch between sender (`uint64` packed) and receiver (string
  UUID in some WSS code paths). Reconciliation pending.
- `bin/handle.py` shebang is hardcoded to a specific venv. Either templatize
  or document the install path during onboarding.
- Bbox-in-frames metadata is not yet implemented. Current path is the radio
  side-channel only.

## Known limitations / open issues

- **Proto/JSON id mismatch.** Receiver-side WSS expects string UUIDs in some
  paths; the sender ships a `uint64` packed id. Reconciliation is pending.
  Tracked in memory.
- **Detector cannot be told to skip.** Object detection runs on raw pixels
  every frame. Suppression is always post-hoc — we remove the metadata, the
  detector still ran.
- **ReID-feature blacklist needs at least one observation.** If a target goes
  INACTIVE before any frame produced an `NVDS_TRACKER_OBJ_REID_META` for it,
  the only suppression key is the tracker-assigned id. Re-entry under a fresh
  id will not be caught.
- **Bbox metadata in RTSP frames.** Not implemented. If the viewer is the web
  client, the recommended path is out-of-band over WebSocket with PTS keyed
  to `requestVideoFrameCallback.mediaTime`. In-band SEI / RTP header
  extensions are options if a richer viewer pipeline is acceptable. See
  conversation history 2026-05-03 for the design discussion.
- **`bin/handle.py` shebang is hardcoded to a Mac path.** Edit before running
  on a different machine, or invoke via `python3` explicitly.

## External references

- `~/deepstream_tracker_meta_reference.md` — DS 7.1 NvDsBatchMeta walks,
  `NvDsObjReid` (`featureSize` / `ptr_host` / `ptr_dev`), tracker config
  examples (OSNet at 512-dim, normalized).
- DeepStream 7.1 headers: `/opt/nvidia/deepstream/deepstream/sources/includes/`
- `swl-vision` — separate repo containing the patched DeepStream app that
  links target-manager.
