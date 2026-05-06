#!/usr/bin/env python3
"""
VLM watchdog: poll /run/swl/crops/ for new <id>.ppm files, run Moondream2 via
llama-mtmd-cli with a JSON-shaped grammar, write the parsed result to <id>.json
next to the PPM. Single-flight (one inference at a time) to avoid CPU
oversubscription against DeepStream.

Per-target lifetime: target_manager writes one PPM per first-seen target id.
This watchdog writes exactly one JSON per PPM (deduped by file presence).
"""

import json
import logging
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

CROPS_DIR  = Path(os.environ.get("VLM_CROPS_DIR",  "/home/swl-jetson-1/swl-vision/run/crops"))
LLAMA_CLI  = os.environ.get("VLM_LLAMA_CLI", "/home/swl-jetson-1/llama.cpp/build/bin/llama-mtmd-cli")
TEXT_MODEL = os.environ.get("VLM_TEXT_MODEL", "/home/swl-jetson-1/swl-vision/models/moondream2/moondream2-text-model-q4_k_m.gguf")
MMPROJ     = os.environ.get("VLM_MMPROJ",     "/home/swl-jetson-1/swl-vision/models/moondream2/moondream2-mmproj-f16.gguf")
GRAMMAR    = os.environ.get("VLM_GRAMMAR",    "/home/swl-jetson-1/xTechHack/bin/vlm_grammar.gbnf")
THREADS    = os.environ.get("VLM_THREADS", "6")
POLL_SEC   = float(os.environ.get("VLM_POLL_SEC", "2.0"))
TIMEOUT_S  = int(os.environ.get("VLM_TIMEOUT_SEC", "180"))

PROMPT = (
    "Question: What kind of vessel is this? "
    'Reply with JSON only: {"class":"cargo|tanker|fishing|small_craft|passenger|tug|naval|yacht|other"}'
    "\n\nAnswer:"
)

# Class → typical overall length in meters. Used as a size prior for
# range-from-angular-subtense in the geolocator. These are coarse medians
# across the most common subtypes; good enough for hackathon scope.
# Source: rough industry medians (containerships ~150-300, tankers ~200-330,
# fishing trawlers ~15-30, tugs ~20-35, mid-size yachts ~10-25, small craft
# ~5-12, passenger ferries ~80-150, naval frigates/corvettes ~100-150).
CLASS_LENGTH_M = {
    "cargo":       150.0,
    "tanker":      220.0,
    "fishing":      22.0,
    "small_craft":   8.0,
    "passenger":   120.0,
    "tug":          28.0,
    "naval":       120.0,
    "yacht":        18.0,
    "other":        30.0,
}

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s vlm_watch: %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger("vlm_watch")

_running = True
def _stop(_sig, _frm):
    global _running
    log.info("shutdown signal received")
    _running = False
signal.signal(signal.SIGINT,  _stop)
signal.signal(signal.SIGTERM, _stop)


def run_inference(ppm_path: Path) -> dict | None:
    """Run llama-mtmd-cli on ppm_path and return parsed JSON dict, or None."""
    env = os.environ.copy()
    env["CUDA_VISIBLE_DEVICES"] = ""  # CPU-only — DS owns the NvMap carveout
    cmd = [
        LLAMA_CLI,
        "-m", TEXT_MODEL,
        "--mmproj", MMPROJ,
        "--image", str(ppm_path),
        "--chat-template", "vicuna",  # lets mtmd-cli load; prompt provides Q/A format
        "--grammar-file", GRAMMAR,
        "-p", PROMPT,
        "-n", "60",
        "--temp", "0",
        "-ngl", "0",
        "--no-warmup",
        "-t", THREADS,
    ]
    t0 = time.time()
    try:
        proc = subprocess.run(
            cmd,
            env=env,
            capture_output=True,
            text=True,
            timeout=TIMEOUT_S,
        )
    except subprocess.TimeoutExpired:
        log.warning("inference timed out after %ds for %s", TIMEOUT_S, ppm_path.name)
        return None
    dt = time.time() - t0

    out = (proc.stdout or "") + (proc.stderr or "")
    m = re.search(r"\{\s*\"class\"\s*:\s*\"([a-z_]+)\"\s*\}", out)
    if not m:
        log.warning("no grammar-shaped JSON in output for %s (%.1fs); proc rc=%d",
                    ppm_path.name, dt, proc.returncode)
        return None
    cls = m.group(1)
    length_m = CLASS_LENGTH_M.get(cls, CLASS_LENGTH_M["other"])
    parsed = {"class": cls, "length_m": length_m}
    log.info("infer %s -> %s (%.1fs)", ppm_path.name, parsed, dt)
    return parsed


def process_one(ppm: Path) -> None:
    sidecar = ppm.with_suffix(".json")
    if sidecar.exists():
        return
    # Atomic-ish marker so a second watcher instance doesn't double-run.
    inflight = ppm.with_suffix(".inflight")
    try:
        # O_EXCL claim
        fd = os.open(str(inflight), os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
        os.close(fd)
    except FileExistsError:
        return

    try:
        result = run_inference(ppm)
        if result is None:
            return
        tmp = sidecar.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(result))
        os.replace(tmp, sidecar)
    finally:
        try:
            inflight.unlink()
        except FileNotFoundError:
            pass


def main() -> int:
    if not CROPS_DIR.exists():
        log.error("crops dir does not exist: %s", CROPS_DIR)
        return 2
    for p in (LLAMA_CLI, TEXT_MODEL, MMPROJ, GRAMMAR):
        if not Path(p).exists():
            log.error("missing artifact: %s", p)
            return 2

    log.info("watching %s (poll=%.1fs, timeout=%ds, threads=%s)",
             CROPS_DIR, POLL_SEC, TIMEOUT_S, THREADS)

    while _running:
        # Sort so older targets get processed first; deterministic for testing.
        for ppm in sorted(CROPS_DIR.glob("*.ppm")):
            if not _running:
                break
            process_one(ppm)
        time.sleep(POLL_SEC)
    log.info("exiting cleanly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
