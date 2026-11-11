"""Replay Studio issue reports and compare clouds with a finer sampling reference.

Run against a workspace copy of the project, with Studio already running:
    python tools/orbit_report_rendering.py --reports 10 11 12 13 --out build/report-fixes/verified
Captures share camera/time/exposure. Reports are never marked resolved by this tool.
"""
from __future__ import annotations

import argparse
import json
import math
import pathlib
import subprocess
import time

import numpy as np
from PIL import Image
import orbit_ab as ab

REFERENCE = "CLOUD_SUN_KNOTS=0;CLOUD_MAX_STEPS=1200;CLOUD_SCHEDULE_STEPS=600;CLOUD_BASE_STEP=15.0;CLOUD_FOOTPRINT_STEP=0.1;CLOUD_INCLOUD_STEP_SCALE=0.25"


def checked_defines(defines: str) -> None:
    result = ab.rpc("clouds.set_defines", {"defines": defines})
    if result.get("error"):
        raise RuntimeError(result["error"])


def restore_report(report: dict) -> None:
    expected = report["start"]["state"]["pose"]
    ab.rpc("reports.restore", {"id": report["id"]})
    deadline = time.monotonic() + 20.0
    while time.monotonic() < deadline:
        pose = ab.rpc("viewport.pose_get", {"id": expected.get("id", "studio.primary")})
        distance = math.dist(pose.get("observer", [0, 0, 0]), expected["observer"])
        if (pose.get("target_object") == expected["target_object"] and distance < 0.01
                and abs(pose["pitch"] - expected["pitch"]) < 1.0e-5
                and abs(pose["yaw"] - expected["yaw"]) < 1.0e-5):
            return
        time.sleep(0.2)
    raise RuntimeError(f"Report {report['id']} camera did not restore; refusing a mismatched capture")


def capture(path: pathlib.Path, width: int, height: int) -> tuple[np.ndarray, float]:
    start = time.monotonic()
    ab.rpc("viewport.capture_start", {"kind": "custom", "width": width,
        "height": height, "path": str(path), "settle_frames": 128})
    while time.monotonic() - start < 90.0:
        status = ab.rpc("viewport.capture_status")
        if status["state"] == "idle":
            last = status.get("last", {})
            if last.get("error"):
                raise RuntimeError(last["error"])
            pixels = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)
            if pixels.shape[:2] != (height, width):
                raise RuntimeError(f"Capture size {pixels.shape[:2]} differs from {(height, width)}")
            return pixels, time.monotonic() - start
        time.sleep(0.1)
    raise RuntimeError("Explicit-resolution viewport capture did not finish")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reports", nargs="+", type=int, required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--reference", default=REFERENCE)
    parser.add_argument("--exposure", type=float, help="Pin a common exposure instead of adapting per report")
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    old_pose = ab.rpc("viewport.pose_get")
    old_time = ab.rpc("time.get")
    old_defines = ab.rpc("clouds.get_defines")["defines"]
    old_eye = ab.rpc("display.eye_get")["config"]
    results = {"project": ab.rpc("project.info"), "reference_defines": args.reference,
               "commit": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
               "dirty": subprocess.check_output(["git", "status", "--short"], text=True), "reports": []}
    try:
        for report_id in args.reports:
            report = ab.rpc("reports.get", {"id": report_id})
            restore_report(report)
            checked_defines("")
            ab.unpin_exposure()
            camera = report["start"]["state"]["camera"]
            width, height = camera["width"], camera["height"]
            capture(out / f"R-{report_id:04d}-warmup.png", width, height)
            exposure = args.exposure if args.exposure is not None else ab.eye_exposure()
            ab.pin_exposure(exposure)
            row = {"id": report_id, "title": report["title"], "exposure": exposure, "variants": {}}
            captures = []
            for variant, defines in [("default", ""), ("reference", args.reference), ("repeat", "")]:
                checked_defines(defines)
                path = out / f"R-{report_id:04d}-{variant}.png"
                restore_report(report)
                pixels, seconds = capture(path, width, height)
                if captures and captures[0].shape != pixels.shape:
                    raise RuntimeError("Viewport resized during capture; rerun after the Studio handoff settles")
                captures.append(pixels)
                profiler = ab.rpc("profiler.status")
                row["variants"][variant] = {"path": str(path), "settle_seconds": seconds,
                    "frame_average_ms": profiler["average_ms"], "frame_worst_ms": profiler["worst_ms"],
                    "frame_median_ms": float(np.median(profiler["recent_frame_ms"]))}
            row["reference_mean_abs_255"] = float(np.abs(captures[0] - captures[1]).mean())
            row["repeat_mean_abs_255"] = float(np.abs(captures[0] - captures[2]).mean())
            row["diagnostics"] = ab.rpc("view.text_diagnostics", {"id": "studio.primary"})
            results["reports"].append(row)
            (out / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
            print(f"R-{report_id:04d}: reference {row['reference_mean_abs_255']:.3f}/255, "
                  f"repeat {row['repeat_mean_abs_255']:.3f}/255", flush=True)
    finally:
        checked_defines(old_defines)
        ab.unpin_exposure()
        ab.rpc("display.eye_set", {"max_exposure_scale": old_eye["max_exposure_scale"]})
        ab.rpc("display.eye_set", {"min_exposure_scale": old_eye["min_exposure_scale"]})
        ab.rpc("time.set", {"time_microseconds": old_time["time_microseconds"],
                            "rate": old_time["rate"], "playing": old_time["playing"]})
        ab.rpc("selection.set", {"ids": [old_pose["target_object"]]})
        ab.rpc("viewport.pose_set", old_pose)


if __name__ == "__main__":
    main()
