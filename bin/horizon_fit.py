#!/usr/bin/env python3
"""
Horizon-line calibration helper.

Grabs a single frame from the live RTSP output of the deepstream container,
detects the sky-water horizon by column-wise vertical-gradient argmin in the
upper band of the frame, and prints the .env values for
`CAM_HORIZON_V_AT_CX` (and `CAM_HORIZON_SLOPE` if --fit-slope is passed).

Usage:
  bin/horizon_fit.py [--rtsp URL] [--out PNG] [--fit-slope]

Defaults to rtsp://127.0.0.1:9554/ds-test and writes a green-line overlay to
/tmp/horizon_overlay.png so you can sanity-check the fit before pasting the
values into swl-vision/.env.

Re-run any time the camera is repointed (heading change ≠ pitch change, but
mount drift can show up either way).
"""

import argparse
import os
import subprocess
import sys
import tempfile

import cv2
import numpy as np


def grab_frame(rtsp: str, out_path: str, retries: int = 3) -> bool:
    for i in range(retries):
        if os.path.exists(out_path):
            os.remove(out_path)
        proc = subprocess.run(
            ["ffmpeg", "-y", "-loglevel", "error", "-i", rtsp,
             "-frames:v", "1", out_path],
            capture_output=True, timeout=20,
        )
        if os.path.exists(out_path) and os.path.getsize(out_path) > 0:
            return True
        print(f"  attempt {i+1}/{retries}: ffmpeg rc={proc.returncode}; "
              f"err={proc.stderr.decode().strip().splitlines()[:2]}",
              file=sys.stderr)
    return False


def fit_horizon(img: np.ndarray, fit_slope: bool = False) -> tuple[float, float, int]:
    """Return (v_at_cx, slope, n_inliers). Search top 25% of frame, mask
    weak-gradient columns (vessel-occluded or uniform sky)."""
    H, W = img.shape[:2]
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    roi_top = int(0.05 * H)
    roi_bot = int(0.25 * H)
    roi = gray[roi_top:roi_bot, :].astype(np.float32)
    roi_blur = cv2.GaussianBlur(roi, (61, 1), 0)
    g = np.diff(roi_blur, axis=0)
    v_per_u = np.argmin(g, axis=0).astype(np.float64) + roi_top
    amp = -g.min(axis=0)
    mask = amp > 0.5 * np.median(amp)
    if mask.sum() < 50:
        raise RuntimeError(f"too few clean columns ({mask.sum()}) for horizon fit")
    if fit_slope:
        u_idx = np.arange(W)
        keep = mask.copy()
        for _ in range(4):
            slope, intercept = np.polyfit(u_idx[keep], v_per_u[keep], 1)
            pred = intercept + slope * u_idx
            resid = v_per_u - pred
            sigma = max(1.5, np.median(np.abs(resid[keep])) * 1.4826)
            keep = mask & (np.abs(resid) < 2.5 * sigma)
        slope, intercept = np.polyfit(u_idx[keep], v_per_u[keep], 1)
        v_at_cx = intercept + slope * (W / 2)
        return v_at_cx, slope, int(keep.sum())
    v_at_cx = float(np.median(v_per_u[mask]))
    return v_at_cx, 0.0, int(mask.sum())


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rtsp", default="rtsp://127.0.0.1:9554/ds-test")
    ap.add_argument("--out",  default="/tmp/horizon_overlay.png")
    ap.add_argument("--fit-slope", action="store_true",
                    help="also fit slope (default: lock 0; assumes CAM_ROLL=0)")
    args = ap.parse_args()

    with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tmp:
        frame_path = tmp.name
    try:
        print(f"grabbing frame from {args.rtsp} ...", file=sys.stderr)
        if not grab_frame(args.rtsp, frame_path):
            print("ERROR: could not grab frame from RTSP", file=sys.stderr)
            return 2
        img = cv2.imread(frame_path)
        if img is None:
            print("ERROR: ffmpeg succeeded but image unreadable", file=sys.stderr)
            return 2
        v_at_cx, slope, n = fit_horizon(img, fit_slope=args.fit_slope)
        H, W = img.shape[:2]
        cx = W / 2
        out = img.copy()
        cv2.line(out,
                 (0, int(v_at_cx - slope * cx)),
                 (W, int(v_at_cx + slope * (W - cx))),
                 (0, 255, 0), 2)
        cv2.imwrite(args.out, out)
        print(f"# horizon fit on {W}x{H} ({n} inlier columns)", file=sys.stderr)
        print(f"# overlay → {args.out}", file=sys.stderr)
        print(f"CAM_HORIZON_V_AT_CX={v_at_cx:.1f}")
        print(f"CAM_HORIZON_SLOPE={slope:.5f}")
        return 0
    finally:
        try: os.remove(frame_path)
        except FileNotFoundError: pass


if __name__ == "__main__":
    sys.exit(main())
