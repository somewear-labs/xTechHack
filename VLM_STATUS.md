# Geolocation / Multi-Scene Status

## ✅ V1 horizon-relative geolocation — SHIPPING

Current Jetson is running stable with horizon-corrected geolocation. Every target gets a valid lat/lon every flush; Beam posts succeeding at 5s cadence with HTTP 200; UDS overlay bridge connected to peer **100.68.81.250**.

Last known good measurement (60s window):
- `geo fail` events: **0**
- Successful flushes: **11**
- Beam http=200 posts: **11**

## Active config

```
swl-vision/.env (active)
  CAM_LAT=1.3008309857682634   CAM_LON=103.9150187520638
  CAM_HEADING=165              CAM_PITCH=0   CAM_ROLL=0   CAM_ALT_M=3
  CAM_FX=4019  CAM_FY=4019  CAM_CX=640  CAM_CY=360
  CAM_HORIZON_V_AT_CX=126      # detected; implies effective pitch ≈3.3° up
  CAM_HORIZON_SLOPE=0.0
```

## Code state (uncommitted on `fix-ui-id-display`)
- `target-manager/geolocation.{hpp,cpp}` — horizon-anchored shift in `pixel_to_gps`; new `pixel_to_gps_from_size` (kept; currently inactive since horizon path always succeeds).
- `target-manager/target_manager.{hpp,cpp}` — `size_priors_m_` + `vlm_loader_loop()` thread + on_batch switch (size-prior preferred when available, else horizon-corrected pixel_to_gps).
- `bin/horizon_fit.py` — RTSP frame grab + Sobel/gradient horizon fit, prints recommended env values.
- `bin/vlm_watch.py` + `bin/vlm_grammar.gbnf` — Moondream2 watchdog. **DEPRECATED** — see pivot below.
- `swl-vision/.env` and `swl-vision/docker-compose.yml` — added `CAM_HORIZON_V_AT_CX` and `CAM_HORIZON_SLOPE`.

## ⚠️ Direction pivot (2026-05-05): VLM → LLM scene parsing

We're stepping away from the Moondream2 VLM watchdog. The replacement direction:

- Operator supplies a per-source descriptor: roughly *where* the camera is and *which way* it's looking.
- An **LLM** (not a VLM) parses scene context from that descriptor and surfaces useful priors / scene info over a message.
- Hook-in mechanism is **not finalized** — likely the existing message bus, possibly a side channel.

**Not implemented yet — this is direction, not a task.** Don't extend `vlm_watch.py` or the `vlm_loader_loop()` size-prior thread; treat them as sunset paths. The per-source descriptor architecture below is the *front* of this pivot — design the descriptor schema to be LLM-friendly.

## VLM artifacts on disk (now sunset)
- `models/moondream2/moondream2-text-model-q4_k_m.gguf` (877 MB) — deletion candidate once watchdog is fully retired
- `models/moondream2/moondream2-mmproj-f16.gguf` (868 MB) — deletion candidate
- `models/moondream2/moondream2-text-model-f16.gguf` (2.7 GB) — deletion candidate; biggest single recovery

Disk: **1.1 GB free**, 99% used. Reclaiming ~4.4 GB by dropping all three is on the table once the LLM path replaces them.

## Pending: Oxford Town Centre test clip

Goal: validate the same horizon-relative pipeline on a non-maritime (pedestrian) scene.

**Canonical sources are dead or throttled, but Kaggle has a live mirror:**
- ✅ **Kaggle: `almightyj/oxford-town-centre`** — has `TownCentreXVID.avi` (342 MB) + `TownCentre-calibration.ci`. Pull with:
  ```bash
  kaggle datasets download -d almightyj/oxford-town-centre -p swl-vision/test-clips/ --unzip
  ```
  (needs `~/.kaggle/kaggle.json`; otherwise download via browser)
- `robots.ox.ac.uk/.../TownCentreXVID.avi` → 404
- `archive.org/download/TownCentreXVID/...` → 503 (throttled)
- Various GitHub mirrors → 404

For any of these: drop the file into `swl-vision/test-clips/`, wire the deepstream `[source0]` to point at it, re-run `bin/horizon_fit.py` for the new view's horizon line, paste new values into `.env`.

## Pending: per-source descriptor architecture (from our conversation)

Cleaner pattern than per-scene `.env` edits — every stream source carries its own descriptor with location + heading + scene hint; edge auto-fills the rest.

Roadmap sketch:
1. **YAML / JSON descriptor per source** (`/run/swl/sources/<id>.yaml`):
   - operator-provided: lat, lon, altitude_m, heading_deg, scene_hint (free text)
   - auto-derived on first frame: horizon line, intrinsics if absent, scene profile from one-shot Moondream call
   - persisted back so restart doesn't re-pay the auto-detect cost
2. **Per-source `CameraGeolocator`** in target_manager: replace single `geo_` with `unordered_map<int, shared_ptr<CameraGeolocator>>` keyed by `NvDsFrameMeta::source_id`. `on_batch` picks the right instance per frame.
3. **`bin/source_register.py`** — one-shot orchestrator: takes operator's minimum descriptor, runs `horizon_fit.py` + Moondream scene-profile, writes complete descriptor.
4. **Multi-source DeepStream config** — multiple `[source0]`/`[source1]` blocks with matching descriptor IDs.

Effort: medium for the C++ refactor (~1 hr), small for descriptor I/O. Worth doing once we want >1 simultaneous source.

## Recovery commands

```bash
# Verify pipeline still healthy
docker ps --filter name=swl-deepstream --format 'table {{.Names}}\t{{.Status}}'
docker logs --since 60s swl-deepstream 2>&1 | grep -c 'http=200'   # expect >0
docker logs --since 60s swl-deepstream 2>&1 | grep -c 'geo fail'   # expect 0

# Watcher state (still running but inactive)
pgrep -af vlm_watch.py

# Re-run horizon calibration after camera repoint
python3 /home/swl-jetson-1/xTechHack/bin/horizon_fit.py
# (paste output into swl-vision/.env, then `docker compose up -d deepstream`)

# Free 2.7 GB if disk gets tight (F16 Moondream no longer needed):
rm /home/swl-jetson-1/swl-vision/models/moondream2/moondream2-text-model-f16.gguf
```

## Currently outbound (confirmed sending)
- ✅ Beam radio via `/api/package/async`
- ✅ UDS overlay bridge → peer 100.68.81.250 (was offline earlier; came online during this session)
- ✅ TM state-change inbound socket listening
- ✅ Beam inbound hook `/tmp/handle_jetson.log` writable

## Recovery notes
- **Beam being flashed on Mac** — SSH path to Jetson goes through that VPN, so connectivity may break. This file is the source of truth for resuming cold.
- Status: V1 horizon-relative geolocation **shipping and stable**. Multi-scene generalization is the next chapter (Town Centre clip + per-source descriptor architecture).
