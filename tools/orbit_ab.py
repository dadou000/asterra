#!/usr/bin/env python3
"""A/B capture harness for Orbit Studio: relaunch per variant, pin the scene, capture, compare.

    python tools/orbit_ab.py run tools/ab_specs/refract_sun.json --project <scratch project dir> [--out DIR] [--exe Orbit.exe]

Why a tool: comparing two renderer settings on screen needs the same ritual every time, and each step has a trap that
silently ruins the comparison (see docs block /tools/ab-capture). This does the ritual:

* kills EVERY Orbit* process first (a hot-generation child `OrbitStudio_hot_*` survives killing `Orbit` and keeps the
  RPC port and the old environment), then launches the variant with exactly its env vars (all other ORBIT_* removed);
* opens the scene's world, pins the simulation clock, places the camera, applies per-scene view layers;
* captures when the image has CONVERGED (two consecutive screenshots within `converge_eps`), not after a fixed sleep;
* relaunches the baseline `controls` more times: the difference between two identical launches is the noise floor;
* writes <out>/<variant>/<scene>.png, report.json (mean/max difference and changed-pixel counts against the baseline,
  next to the control noise) and sheet.png (variants side by side with a 5x difference column).

Only the loopback JSON-RPC on 127.0.0.1:4320 is used; nothing simulates mouse or keyboard input. Use a scratch copy of the
project: world edits and the simulation clock are saved when Studio closes.
"""
from __future__ import annotations

import argparse
import itertools
import json
import math
import os
import pathlib
import socket
import subprocess
import sys
import time

import numpy as np
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
HOST, PORT = "127.0.0.1", int(os.environ.get("ORBIT_RPC_PORT", "4320"))
_ids = itertools.count(1)


def rpc(method: str, params: dict | None = None, timeout: float = 120.0):
    request_id = next(_ids)
    with socket.create_connection((HOST, PORT), timeout=timeout) as sock:
        sock.sendall((json.dumps({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params or {}}) + "\n").encode())
        buf = b""
        reply = None
        while reply is None:
            chunk = sock.recv(65536)
            if not chunk:
                break
            buf += chunk
            # A reply is one line; other lines (notifications, a late reply to an earlier call) are skipped.
            for line in buf.split(b"\n")[:-1]:
                if not line.strip():
                    continue
                try:
                    candidate = json.loads(line)
                except ValueError:
                    continue
                if candidate.get("id") == request_id:
                    reply = candidate
                    break
    if reply is None:
        raise RuntimeError(f"{method}: no reply with id {request_id}")
    if "error" in reply:
        raise RuntimeError(f"{method}: {reply['error']}")
    return reply["result"]


def kill_orbit() -> None:
    subprocess.run(["powershell", "-NoProfile", "-Command", "Get-Process Orbit* -ErrorAction SilentlyContinue | Stop-Process -Force"],
                   capture_output=True)
    time.sleep(2.0)


def launch(exe: pathlib.Path, project: pathlib.Path, env_vars: dict[str, str]) -> subprocess.Popen:
    kill_orbit()
    env = {k: v for k, v in os.environ.items() if not k.startswith("ORBIT_")}
    env.update({k: str(v) for k, v in env_vars.items()})
    proc = subprocess.Popen([str(exe), str(project)], env=env, cwd=str(ROOT))
    deadline = time.time() + 180
    while time.time() < deadline:
        try:
            rpc("studio.process_info", timeout=3)
            return proc
        except Exception:
            time.sleep(2.0)
    raise RuntimeError("Studio did not answer on the RPC port within 180 s")


def find_object(name: str) -> str:
    def walk(parent: str | None, depth: int):
        kids = rpc("object.roots") if parent is None else rpc("object.children", {"parent": parent})
        for kid in kids:
            if kid["name"] == name:
                return kid["id"]
            if depth < 4:
                found = walk(kid["id"], depth + 1)
                if found:
                    return found
        return None
    found = walk(None, 0)
    if not found:
        raise RuntimeError(f"no object named {name!r}")
    return found


def lat_lon_up(lat_deg: float, lon_deg: float) -> np.ndarray:
    la, lo = math.radians(lat_deg), math.radians(lon_deg)
    return np.array([math.cos(la) * math.cos(lo), math.sin(la), -math.cos(la) * math.sin(lo)])


def surface_frame(up: np.ndarray):
    north = np.array([0.0, 1.0, 0.0]) - up * up[1]
    north /= np.linalg.norm(north)
    return north, np.cross(north, up)


def set_pose(base: dict, observer: np.ndarray, yaw: float, pitch: float) -> None:
    up = observer / np.linalg.norm(observer)
    north, east = surface_frame(up)
    pose = dict(base)
    pose.update(observer=observer.tolist(), up=up.tolist(), north=north.tolist(), east=east.tolist(), yaw=yaw, pitch=pitch)
    rpc("viewport.pose_set", pose)


def apply_pose(spec: dict, radius: float) -> None:
    base = rpc("viewport.pose_get")
    kind = spec.get("kind", "surface")
    if kind == "surface":
        up = lat_lon_up(spec["lat_deg"], spec["lon_deg"])
        set_pose(base, up * (radius + spec["alt_m"]), math.radians(spec.get("yaw_deg", 0.0)), math.radians(spec.get("pitch_deg", 0.0)))
    elif kind == "approach":
        # Observer on the line from a target point on the surface through an origin point, scaled to alt_km,
        # looking at the target: the same patch of ground at every altitude.
        t = lat_lon_up(spec["target"]["lat_deg"], spec["target"]["lon_deg"]) * radius
        o = lat_lon_up(spec["origin"]["lat_deg"], spec["origin"]["lon_deg"]) * (radius + spec["origin"]["alt_km"] * 1000.0)
        direction = (o - t) / np.linalg.norm(o - t)
        lo, hi = 0.0, 1.0e9
        for _ in range(80):
            mid = 0.5 * (lo + hi)
            lo, hi = (mid, hi) if np.linalg.norm(t + mid * direction) - radius < spec["alt_km"] * 1000.0 else (lo, mid)
        observer = t + lo * direction
        up = observer / np.linalg.norm(observer)
        north, east = surface_frame(up)
        look = (t - observer) / np.linalg.norm(t - observer)
        set_pose(base, observer, math.atan2(look @ east, look @ north), math.asin(max(-1.0, min(1.0, look @ up))))
    else:
        raise ValueError(f"unknown pose kind {kind!r}")


def grab(path: pathlib.Path) -> np.ndarray:
    rpc("viewport.screenshot", {"path": str(path).replace("\\", "/")})
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)


def profiler_for_passes(spec: dict) -> None:
    """Live window of gpu_window_ms and no freeze on hitch: a frozen panel returns the same snapshot for every scene."""
    rpc("profiler.panel_set", {"window_ms": float(spec.get("gpu_window_ms", 2000)), "freeze_on_hitch": False, "paused": False})


def gpu_pass_ms(names: list[str], window_ms: float) -> dict[str, float]:
    """Average GPU ms per frame of the named passes over the profiler's last `window_ms`, from ONE snapshot.

    profiler.snapshot's top_gpu_passes is limited to the panel's visible time range (a rolling window), so two snapshots
    cannot be subtracted (that gave negative times); one snapshot taken after the window has filled with the current scene
    is a valid average. Passes outside the top list read as 0.
    """
    time.sleep(window_ms / 1000.0 + 0.5)
    rows = {row["name"].split(".")[-1]: row for row in rpc("profiler.snapshot").get("top_gpu_passes", [])}
    return {n: round(rows[n]["total_ms"] / max(rows[n]["count"], 1), 3) if n in rows else 0.0 for n in names}


def capture_converged(path: pathlib.Path, eps: float, min_wait: float, max_wait: float, step: float) -> tuple[np.ndarray, float]:
    """Screenshots until two consecutive frames agree within eps (mean abs, 0..255); returns the frame and seconds used."""
    start = time.time()
    time.sleep(min_wait)
    previous = grab(path)
    stable = 0
    while time.time() - start < max_wait:
        time.sleep(step)
        current = grab(path)
        stable = stable + 1 if previous.shape == current.shape and float(np.abs(current - previous).mean()) < eps else 0
        previous = current
        if stable >= 2:
            break
    return previous, time.time() - start


def load_variant(label: str, spec: dict, out: pathlib.Path):
    """Previously captured variant (all scene PNGs and frame_ms.json present), or None."""
    meta = out / label / "frame_ms.json"
    names = [scene["name"] for scene in spec["scenes"]]
    if not meta.exists() or not all((out / label / f"{n}.png").exists() for n in names):
        return None
    loaded = json.loads(meta.read_text(encoding="utf-8"))
    frame_ms = loaded if "frame_ms" in loaded and "gpu_ms" in loaded else {"frame_ms": loaded, "gpu_ms": {}}
    return {n: np.asarray(Image.open(out / label / f"{n}.png").convert("RGB"), dtype=np.float32) for n in names}, frame_ms


def run_variant(label: str, env_vars: dict, spec: dict, exe: pathlib.Path, out: pathlib.Path) -> tuple[dict[str, np.ndarray], dict]:
    project = pathlib.Path(spec["project"]).resolve()
    launch(exe, project, env_vars)
    rpc("studio.panel_focus", {"title": "Viewport"})
    world = spec.get("world")
    if world and world not in rpc("world.active").get("path", ""):
        rpc("world.open", {"path": world})
        time.sleep(15.0)
    body = find_object(spec.get("body", "Earth"))
    radius = float(spec.get("radius_m", 6371000.0))
    rpc("selection.set", {"ids": [body]})
    rpc("viewport.focus_body")
    time.sleep(6.0)
    images: dict[str, np.ndarray] = {}
    frame_ms: dict[str, float] = {}
    gpu_ms: dict[str, dict] = {}
    if spec.get("gpu_passes"):
        profiler_for_passes(spec)
    (out / label).mkdir(parents=True, exist_ok=True)
    for scene in spec["scenes"]:
        if "time_microseconds" in scene:
            rpc("time.set", {"playing": False, "time_microseconds": scene["time_microseconds"]})
        if "local_time_seconds" in scene:
            rpc("time.set", {"playing": False, "local_time_seconds": float(scene["local_time_seconds"])})
        if scene.get("layers"):
            rpc("view.terrain_layers_set", {"id": "studio.primary", **scene["layers"]})
        apply_pose(scene["pose"], radius)
        image, used = capture_converged(out / label / f"{scene['name']}.png", spec.get("converge_eps", 0.08),
                                        scene.get("min_wait", 1.5), scene.get("max_wait", 45.0), spec.get("poll_step", 0.6))
        images[scene["name"]] = image
        recent = rpc("profiler.status").get("recent_frame_ms") or []
        frame_ms[scene["name"]] = round(float(np.median(recent)), 2) if recent else 0.0
        if spec.get("gpu_passes"):
            gpu_ms[scene["name"]] = gpu_pass_ms(spec["gpu_passes"], float(spec.get("gpu_window_ms", 2000)))
        print(f"  {label:10s} {scene['name']:18s} converged in {used:4.1f} s  frame {frame_ms[scene["name"]]:6.2f} ms  gpu {gpu_ms.get(scene["name"], "")}", flush=True)
    (out / label / "frame_ms.json").write_text(json.dumps({"frame_ms": frame_ms, "gpu_ms": gpu_ms}), encoding="utf-8")
    return images, {"frame_ms": frame_ms, "gpu_ms": gpu_ms}


def prepare_session(spec: dict) -> float:
    """World, body, selection; returns the body radius."""
    rpc("studio.panel_focus", {"title": "Viewport"})
    world = spec.get("world")
    if world and world not in rpc("world.active").get("path", ""):
        rpc("world.open", {"path": world})
        time.sleep(15.0)
    body = find_object(spec.get("body", "Earth"))
    rpc("selection.set", {"ids": [body]})
    rpc("viewport.focus_body")
    time.sleep(2.5)
    return float(spec.get("radius_m", 6371000.0))


def set_scene(scene: dict, radius: float) -> None:
    if "time_microseconds" in scene:
        rpc("time.set", {"playing": False, "time_microseconds": scene["time_microseconds"]})
    if "local_time_seconds" in scene:
        rpc("time.set", {"playing": False, "local_time_seconds": float(scene["local_time_seconds"])})
    if scene.get("layers"):
        rpc("view.terrain_layers_set", {"id": "studio.primary", **scene["layers"]})
    apply_pose(scene["pose"], radius)


def shrink(image: np.ndarray, k: int = 4) -> np.ndarray:
    """Box-average by k: motion metrics run on this so a path of 24 frames stays in memory."""
    height, width, _ = image.shape
    return image[: height // k * k, : width // k * k].reshape(height // k, k, width // k, k, 3).mean(axis=(1, 3))


def crop_box(image: np.ndarray, scene: dict) -> np.ndarray:
    """scene["box_rel"] = [x0, y0, x1, y1] in 0..1 restricts the motion metrics to a region (the cloud band, not the sea)."""
    box = scene.get("box_rel")
    if not box:
        return image
    height, width, _ = image.shape
    return image[int(box[1] * height): int(box[3] * height), int(box[0] * width): int(box[2] * width)]


def aligned_error(frame: np.ndarray, reference: np.ndarray, threshold: float) -> tuple[float, float, float]:
    """Mean abs error, share of pixels over `threshold` and the exposure gain, after scaling the frame to the reference's mean.

    Auto-exposure adapts over seconds while the path runs; without this the error is dominated by the adaptation state the
    previous variant left behind (visible as a decay over the first steps of every curve), not by the temporal technique.
    """
    gain = float(reference.mean() / max(float(frame.mean()), 1.0e-6))
    diff = np.abs(frame * gain - reference)
    return float(diff.mean()), float((diff.max(axis=2) > threshold).mean()), abs(gain - 1.0)


def frames_now() -> int:
    return int(rpc("profiler.status").get("frames", 0))


def wait_frames(count: int, timeout: float = 2.0) -> int:
    """Block until the renderer has finished `count` more frames; returns how many passed."""
    start = frames_now()
    deadline = time.time() + timeout
    while frames_now() < start + count and time.time() < deadline:
        time.sleep(0.003)
    return frames_now() - start


def motion_pose(pose: dict, motion: dict, i: int, radius: float) -> dict:
    """The camera pose at path step i. Surface poses: pan / pitch (degrees per step), ascend (metres per step) or move (metres per
    step along the heading, at constant altitude). Approach poses: alt_km changes by per_step per step (negative descends)."""
    p = dict(pose)
    per = float(motion.get("per_step", 0.5))
    kind = motion.get("kind", "pan")
    if p.get("kind", "surface") == "approach":
        p["alt_km"] = float(p["alt_km"]) + per * i
        return p
    if kind == "pan":
        p["yaw_deg"] = float(p.get("yaw_deg", 0.0)) + per * i
    elif kind == "pitch":
        p["pitch_deg"] = float(p.get("pitch_deg", 0.0)) + per * i
    elif kind == "ascend":
        p["alt_m"] = float(p["alt_m"]) + per * i
    elif kind == "move":
        up = lat_lon_up(p["lat_deg"], p["lon_deg"])
        north, east = surface_frame(up)
        yaw = math.radians(float(p.get("yaw_deg", 0.0)))
        moved = up * (radius + float(p["alt_m"])) + (math.cos(yaw) * north + math.sin(yaw) * east) * per * i
        moved /= np.linalg.norm(moved)
        p["lat_deg"] = math.degrees(math.asin(moved[1]))
        p["lon_deg"] = math.degrees(math.atan2(-moved[2], moved[0]))
    else:
        raise ValueError(f"unknown motion kind {kind!r}")
    return p


def reference_pass(scene: dict, motion: dict, radius: float, tmp: pathlib.Path, spec: dict) -> list[np.ndarray]:
    """What each pose should look like with all the history in the world: wait `reference_settle` frames at the pose, then
    average `reference_frames` consecutive frames (so the reference's own temporal noise is negligible). Stopping at the first
    stable full-frame mean is not enough: the cloud band is a small part of the frame and keeps converging after that."""
    settle = int(spec.get("reference_settle", 60))
    count = int(spec.get("reference_frames", 6))
    frames = []
    for i in range(int(motion.get("steps", 24))):
        apply_pose(motion_pose(scene["pose"], motion, i, radius), radius)
        wait_frames(settle, timeout=settle / 20.0 + 3.0)
        stack = []
        for _ in range(count):
            wait_frames(1)
            stack.append(crop_box(shrink(grab(tmp)), scene))
        frames.append(np.mean(np.stack(stack), axis=0))
    return frames


def motion_pass(scene: dict, motion: dict, radius: float, tmp: pathlib.Path, spec: dict):
    """The path in motion: the pose advances every `frames_per_step` rendered frames and the frame is grabbed right after."""
    steps = int(motion.get("steps", 24))
    per = int(motion.get("frames_per_step", 1))
    apply_pose(scene["pose"], radius)
    wait_frames(int(spec.get("warmup_frames", 150)), timeout=15.0)   # a fixed count: a slow history (weight 0.1) needs more than "stable mean"
    frames, since, middle = [], [], None
    for i in range(steps):
        apply_pose(motion_pose(scene["pose"], motion, i, radius), radius)
        before = frames_now()
        wait_frames(per)
        image = grab(tmp)
        since.append(frames_now() - before)
        frames.append(crop_box(shrink(image), scene))
        if i == steps // 2:
            middle = image
    recent = rpc("profiler.status").get("recent_frame_ms") or []
    return frames, middle, since, round(float(np.median(recent)), 2) if recent else 0.0


def flicker_pass(scene: dict, radius: float, tmp: pathlib.Path, spec: dict, count: int = 10) -> float:
    """Static camera, consecutive frames: mean over pixels of the standard deviation (0..255) = temporal noise."""
    apply_pose(scene["pose"], radius)
    wait_frames(int(spec.get("warmup_frames", 150)), timeout=15.0)
    stack = []
    for _ in range(count):
        wait_frames(1)
        stack.append(crop_box(shrink(grab(tmp)), scene))
    return round(float(np.mean(np.std(np.stack(stack), axis=0))), 4)


def run_motion_scene(spec, scene, order, baseline, radius, rounds, out, images, control, frame_rounds, motion_rounds):
    """Motion protocol for one scene across every variant (rounds interleaved), against a baseline-shader reference path."""
    variants = spec["variants"]
    name = scene["name"]
    motion = scene["motion"]
    threshold = float(spec.get("ghost_threshold", 3))
    tmp = out / "_motion_tmp.png"
    set_scene(scene, radius)

    def use(v: str) -> None:
        text = variants[v].get("env", {}).get("ORBIT_CLOUD_DEFINES", "")
        reply = rpc("clouds.set_defines", {"defines": text})
        if reply.get("error"):
            raise RuntimeError(f"clouds.set_defines({text!r}): {reply['error']}")

    use(baseline)
    print(f"  reference path for {name}: {int(motion.get('steps', 24))} converged poses", flush=True)
    reference = reference_pass(scene, motion, radius, tmp, spec)
    for r in range(rounds):
        for v in (order if r % 2 == 0 else list(reversed(order))):
            use(v)
            frames, middle, since, ms = motion_pass(scene, motion, radius, tmp, spec)
            flicker = flicker_pass(scene, radius, tmp, spec)
            scored = [aligned_error(f, g, threshold) for f, g in zip(frames, reference)]
            err = [s[0] for s in scored]
            ghost = [s[1] for s in scored]
            record = {"err_mean": round(float(np.mean(err[1:])), 4), "err_max": round(float(np.max(err[1:])), 4),
                      "ghost_frac": round(float(np.mean(ghost[1:])), 5), "flicker": flicker,
                      "exposure_drift": round(float(np.mean([s[2] for s in scored])), 4),
                      "frames_until_grab": round(float(np.mean(since)), 2), "err_by_step": [round(e, 3) for e in err]}
            motion_rounds[v].setdefault(name, []).append(record)
            frame_rounds[v].setdefault(name, []).append(ms)
            if r == 0:
                images[v][name] = middle
                Image.fromarray(middle.astype("uint8")).save(out / v / f"{name}.png")
            elif r == 1 and v == baseline:
                control[name] = middle
            print(f"  r{r} {v:12s} {name:18s} motion err {record['err_mean']:6.3f}/255 (max {record['err_max']:6.3f}) "
                  f"ghost {100 * record['ghost_frac']:5.2f}%  flicker {flicker:6.3f}  exposure drift {100 * record['exposure_drift']:4.2f}%  frame {ms} ms",
                  flush=True)
    return reference


def summarise_motion(rounds_by_variant: dict) -> dict:
    """Mean over rounds of each metric; `spread` is max - min over rounds for err_mean (the metric's own noise)."""
    out = {}
    for name, rows in rounds_by_variant.items():
        out[name] = {"err_mean": round(float(np.mean([r["err_mean"] for r in rows])), 4),
                     "err_max": round(float(np.mean([r["err_max"] for r in rows])), 4),
                     "ghost_frac": round(float(np.mean([r["ghost_frac"] for r in rows])), 5),
                     "flicker": round(float(np.mean([r["flicker"] for r in rows])), 4),
                     "exposure_drift": round(float(np.mean([r["exposure_drift"] for r in rows])), 4),
                     "frames_until_grab": round(float(np.mean([r["frames_until_grab"] for r in rows])), 2),
                     "err_spread": round(float(max(r["err_mean"] for r in rows) - min(r["err_mean"] for r in rows)), 4),
                     "err_by_step": [round(float(np.mean([r["err_by_step"][i] for r in rows])), 3) for i in range(len(rows[0]["err_by_step"]))]}
    return out


def motion_plot(out: pathlib.Path, scene: str, summary: dict) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    figure, axes = plt.subplots(1, 2, figsize=(11, 3.6), gridspec_kw={"width_ratios": [3, 1.4]})
    for v, entry in summary.items():
        axes[0].plot(entry["err_by_step"], label=f"{v} (mean {entry['err_mean']:.2f})")
    axes[0].set_title(f"{scene}: error of each in-motion frame vs the converged pose (/255)")
    axes[0].set_xlabel("path step")
    axes[0].legend(fontsize=7)
    axes[1].bar(range(len(summary)), [e["flicker"] for e in summary.values()])
    axes[1].set_xticks(range(len(summary)), list(summary), rotation=60, fontsize=7)
    axes[1].set_title("static flicker (std /255)")
    figure.tight_layout()
    figure.savefig(out / f"motion_{scene}.png", dpi=90)
    plt.close(figure)


def run_in_process(spec: dict, exe: pathlib.Path, out: pathlib.Path, order: list[str], baseline: str):
    """One Studio launch for every variant: clouds.set_defines swaps the cloud shader defines in place.

    Rounds alternate direction (A B C, then C B A) so launch-order and thermal drift is spread evenly over the variants, and
    the baseline's own round-to-round spread is the noise floor (no relaunch needed). Only variants that differ by
    ORBIT_CLOUD_DEFINES can be compared this way; flags read once at startup need the relaunch mode.
    """
    variants = spec["variants"]
    base_env = {k: v for k, v in variants[baseline].get("env", {}).items() if k != "ORBIT_CLOUD_DEFINES"}
    launch(exe, pathlib.Path(spec["project"]).resolve(), base_env)
    radius = prepare_session(spec)
    if spec.get("gpu_passes"):
        profiler_for_passes(spec)
    rounds = int(spec.get("rounds", 2))
    images: dict[str, dict] = {v: {} for v in order}
    control: dict[str, np.ndarray] = {}
    frame_rounds: dict[str, dict[str, list]] = {v: {} for v in order}
    gpu_rounds: dict[str, dict[str, list]] = {v: {} for v in order}
    for v in order:
        (out / v).mkdir(parents=True, exist_ok=True)
    motion_rounds: dict[str, dict[str, list]] = {v: {} for v in order}
    for scene in spec["scenes"]:
        name = scene["name"]
        if scene.get("motion"):
            run_motion_scene(spec, scene, order, baseline, radius, rounds, out, images, control, frame_rounds, motion_rounds)
            continue
        set_scene(scene, radius)
        for r in range(rounds):
            for v in (order if r % 2 == 0 else list(reversed(order))):
                define_text = variants[v].get("env", {}).get("ORBIT_CLOUD_DEFINES", "")
                reply = rpc("clouds.set_defines", {"defines": define_text})
                if reply.get("error"):
                    raise RuntimeError(f"clouds.set_defines({define_text!r}): {reply['error']}")
                path = out / v / (f"{name}.png" if r == 0 else f"{name}_r{r}.png")
                image, used = capture_converged(path, spec.get("converge_eps", 0.08), scene.get("min_wait", 1.0),
                                                scene.get("max_wait", 30.0), spec.get("poll_step", 0.5))
                time.sleep(0.6)
                recent = rpc("profiler.status").get("recent_frame_ms") or []
                frame_rounds[v].setdefault(name, []).append(round(float(np.median(recent)), 2) if recent else 0.0)
                if spec.get("gpu_passes"):
                    gpu_rounds[v].setdefault(name, []).append(gpu_pass_ms(spec["gpu_passes"], float(spec.get("gpu_window_ms", 2000))))
                if r == 0:
                    images[v][name] = image
                elif r == 1 and v == baseline:
                    control[name] = image
                print(f"  r{r} {v:12s} {name:18s} converged in {used:4.1f} s  frame {frame_rounds[v][name][-1]:6.2f} ms"
                      + (f"  gpu {gpu_rounds[v][name][-1]}" if spec.get("gpu_passes") else ""), flush=True)
    names = [s["name"] for s in spec["scenes"]]
    frame = {}
    for v in order:
        gpu_mean = {}
        for n in names:
            rows = gpu_rounds[v].get(n, [])
            gpu_mean[n] = {p: round(float(np.mean([row[p] for row in rows])), 3) for p in rows[0]} if rows else {}
        frame[v] = {"frame_ms": {n: round(float(np.mean(frame_rounds[v][n])), 2) for n in names}, "gpu_ms": gpu_mean,
                    "rounds_ms": {n: frame_rounds[v][n] for n in names}, "motion": summarise_motion(motion_rounds[v])}
    spread = {n: round(max(frame_rounds[baseline][n]) - min(frame_rounds[baseline][n]), 2) for n in names}
    if rounds < 2 or len(control) < len(names):
        return images, frame, []   # one round: no repeat of the baseline to serve as the noise floor
    control_name = f"{baseline}_control1"
    images[control_name] = control
    frame[control_name] = {"frame_ms": {n: round(frame[baseline]["frame_ms"][n] + spread[n], 2) for n in names}, "gpu_ms": {},
                           "rounds_ms": {}}
    return images, frame, [control_name]


def metrics(a: np.ndarray, b: np.ndarray, box: list[int] | None, threshold: float) -> dict:
    if a.shape != b.shape:
        return {"error": f"image sizes differ {a.shape[:2]} vs {b.shape[:2]}"}
    if box:
        x, y, w, h = box
        a, b = a[y:y + h, x:x + w], b[y:y + h, x:x + w]
    d = np.abs(a - b)
    return {"mean": round(float(d.mean()), 4), "max": float(d.max()), "px_over": int((d.max(axis=2) > threshold).sum())}


def make_sheet(out: pathlib.Path, labels: list[str], scenes: list[str], images: dict, baseline: str) -> None:
    width = max(300, min(560, 2400 // max(len(labels), 1)))
    blocks = []
    for scene in scenes:
        cells, diffs = [], []
        for label in labels:
            im = Image.fromarray(images[label][scene].astype("uint8"))
            cells.append(im.resize((width, int(im.height * width / im.width))))
            if label != baseline and images[label][scene].shape == images[baseline][scene].shape:
                d = Image.fromarray(np.clip(np.abs(images[label][scene] - images[baseline][scene]) * 5.0, 0, 255).astype("uint8"))
                diffs.append(d.resize(cells[-1].size))
            else:
                diffs.append(Image.new("RGB", cells[-1].size, (0, 0, 0)))
        height = cells[0].height
        block = Image.new("RGB", (width * len(labels), height * 2))
        for i, (c, d) in enumerate(zip(cells, diffs)):
            block.paste(c, (i * width, 0))
            block.paste(d, (i * width, height))
        blocks.append(block)
    sheet = Image.new("RGB", (blocks[0].width, sum(b.height for b in blocks)))
    y = 0
    for b in blocks:
        sheet.paste(b, (0, y))
        y += b.height
    sheet.save(out / "sheet.png")


def sha256(path: pathlib.Path) -> str:
    import hashlib
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git(*args: str) -> str:
    r = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True)
    return r.stdout.strip()


def provenance(exe: pathlib.Path) -> dict:
    gpu = ""
    try:
        gpu = subprocess.run(["nvidia-smi", "--query-gpu=name", "--format=csv,noheader"], capture_output=True, text=True).stdout.strip()
    except OSError:
        pass
    return {"commit": git("rev-parse", "HEAD"), "branch": git("rev-parse", "--abbrev-ref", "HEAD"),
            "dirty_files": [l[3:] for l in git("status", "--porcelain").splitlines()],
            "exe": str(exe), "exe_sha256": sha256(exe), "exe_mtime": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(exe.stat().st_mtime)),
            "gpu": gpu, "date": time.strftime("%Y-%m-%d %H:%M:%S")}


def run_relaunch(spec: dict, exe: pathlib.Path, out: pathlib.Path, order: list[str], baseline: str, resume: bool):
    """One Studio launch per variant (any env var can differ) plus the repeat-launch controls."""
    variants = spec["variants"]
    images: dict[str, dict] = {}
    frame: dict[str, dict] = {}
    for label in order:
        cached = load_variant(label, spec, out) if resume else None
        if cached:
            print(f"[{label}] reused from {out / label}", flush=True)
            images[label], frame[label] = cached
            continue
        print(f"[{label}] env={variants[label].get('env', {})}", flush=True)
        images[label], frame[label] = run_variant(label, variants[label].get("env", {}), spec, exe, out)
    controls = []
    for i in range(int(spec.get("controls", 1))):
        name = f"{baseline}_control{i + 1}"
        cached = load_variant(name, spec, out) if resume else None
        if cached:
            print(f"[{name}] reused", flush=True)
            images[name], frame[name] = cached
        else:
            print(f"[{name}] noise control", flush=True)
            images[name], frame[name] = run_variant(name, variants[baseline].get("env", {}), spec, exe, out)
        controls.append(name)
    return images, frame, controls


def cmd_run(args) -> int:
    spec = json.loads(pathlib.Path(args.spec).read_text(encoding="utf-8"))
    spec["project"] = args.project or os.environ.get("ORBIT_AB_PROJECT") or spec.get("project")
    if not spec["project"]:
        raise SystemExit("give a scratch project: --project DIR, ORBIT_AB_PROJECT or \"project\" in the spec")
    out = pathlib.Path(args.out or ROOT / "build" / "ab" / pathlib.Path(args.spec).stem).resolve()
    out.mkdir(parents=True, exist_ok=True)
    exe = pathlib.Path(args.exe or ROOT / "Orbit.exe").resolve()
    variants: dict = spec["variants"]
    baseline = spec.get("baseline", next(iter(variants)))
    order = [baseline] + [v for v in variants if v != baseline]
    started = time.time()
    try:
        if spec.get("mode") == "in_process":
            images, frame, controls = run_in_process(spec, exe, out, order, baseline)
        else:
            images, frame, controls = run_relaunch(spec, exe, out, order, baseline, args.resume)
    finally:
        kill_orbit()
    threshold = float(spec.get("changed_pixel_threshold", 12))
    report = {"baseline": baseline, "seconds": round(time.time() - started, 1), "provenance": provenance(exe),
              "variants": {v: variants[v].get("env", {}) for v in order}, "spec": spec, "scenes": {}}
    for scene in spec["scenes"]:
        name, box = scene["name"], scene.get("box")
        entry = {"noise": [metrics(images[baseline][name], images[c][name], box, threshold) for c in controls]}
        entry["variants"] = {v: metrics(images[baseline][name], images[v][name], box, threshold) for v in order[1:]}
        entry["frame_ms"] = {v: frame[v]["frame_ms"][name] for v in order}
        if any(name in frame[v].get("motion", {}) for v in order):
            entry["motion"] = {v: frame[v]["motion"][name] for v in order if name in frame[v].get("motion", {})}
        entry["gpu_pass_ms"] = {v: frame[v]["gpu_ms"].get(name, {}) for v in order}
        entry["gpu_pass_ms_repeat"] = [frame[c]["gpu_ms"].get(name, {}) for c in controls]
        # Launch-to-launch drift (GPU clocks, order): the same build measured again. Cost differences below it are not resolved.
        entry["frame_ms_drift"] = [round(frame[c]["frame_ms"][name] - frame[baseline]["frame_ms"][name], 2) for c in controls]
        entry["image_sha256"] = {v: sha256(out / v / f"{name}.png") for v in order}
        entry["resolution"] = list(images[baseline][name].shape[:2])
        report["scenes"][name] = entry
    for name, entry in report["scenes"].items():
        if entry.get("motion"):
            motion_plot(out, name, entry["motion"])
    (out / "report.json").write_text(json.dumps(report, indent=1), encoding="utf-8")
    make_sheet(out, order, [s["name"] for s in spec["scenes"]], images, baseline)
    print(f"\nbaseline={baseline}  total {report['seconds']} s  ->  {out}")
    for name, entry in report["scenes"].items():
        for v, m in (entry.get("motion") or {}).items():
            floor = (entry["motion"].get(baseline) or {}).get("err_spread", 0.0)
            print(f"  motion {name:16s} {v:12s} err {m['err_mean']:7.3f} (max {m['err_max']:7.3f}) ghost {100 * m['ghost_frac']:6.2f}% "
                  f"flicker {m['flicker']:7.3f}  exposure drift {100 * m['exposure_drift']:5.2f}%  [baseline err spread over rounds {floor:.3f}]")
    for name, entry in report["scenes"].items():
        noise = max((n.get("mean", 0.0) for n in entry["noise"]), default=0.0)
        noise_px = max((n.get("px_over", 0) for n in entry["noise"]), default=0)
        for v, m in entry["variants"].items():
            # A mean hides a localised change (a thinned cloud bank is a few percent of the frame), so the changed-pixel
            # count against the repeat launch counts too.
            by_mean = m.get("mean", 0.0) > max(3.0 * noise, noise + 0.05)
            by_pixels = m.get("px_over", 0) > 3 * noise_px + 500
            verdict = "DIFFERENT" if (by_mean or by_pixels) else "same as noise"
            print(f"  {name:18s} {v:10s} mean {m.get('mean')}  max {m.get('max')}  px>{threshold:g}: {m.get('px_over')}  (noise mean {noise:.3f})  {verdict}  frame {entry['frame_ms'][v]} vs {entry['frame_ms'][baseline]} ms (drift of the same build: {entry['frame_ms_drift']})")
            if entry.get("gpu_pass_ms", {}).get(v):
                print(f"{'':30s} gpu passes ms: {entry['gpu_pass_ms'][v]} vs baseline {entry['gpu_pass_ms'][baseline]} (repeat launch {entry['gpu_pass_ms_repeat']})")
    return 0


REVIEWS = ROOT / "docs" / "reviews"
RUBRIC = "v1"


def cmd_council(args) -> int:
    """Persist a council review: the run's report and thumbnail sheet plus the lenses' findings and ratings.

    findings JSON: {title, question, chosen, verdict, lenses: [{name, findings, ratings: {variant: {category: [score|null, confidence, why]}}}],
    disagreements, hypotheses, verified_facts, open_issues, recommended_changes, completed_changes}. Categories are the rubric v1
    categories in docs/reviews/RUBRIC.md; null = N/A. Subjective ratings are kept apart from the measured metrics in the report.
    """
    out = pathlib.Path(args.out_dir).resolve()
    report = json.loads((out / "report.json").read_text(encoding="utf-8"))
    council = json.loads(pathlib.Path(args.findings).read_text(encoding="utf-8"))
    commit = report["provenance"]["commit"][:8]
    review_id = f"R-{time.strftime('%Y%m%d-%H%M%S')}-{commit}"
    target = REVIEWS / review_id
    if target.exists():
        raise SystemExit(f"{target} exists; reviews are immutable")
    target.mkdir(parents=True)
    sheet = Image.open(out / "sheet.png").convert("RGB")
    if sheet.width > 1800:
        sheet = sheet.resize((1800, int(sheet.height * 1800 / sheet.width)))
    sheet.save(target / "sheet.png", optimize=True)
    record = {"id": review_id, "rubric": RUBRIC, "council": council, "run": report,
              "images": "full-resolution PNGs stay outside git (see run.scenes[*].image_sha256 and the capture folder at "
                        f"{out}); sheet.png here is a downscaled contact sheet (top row variants, bottom row 5x difference vs baseline)"}
    (target / "review.json").write_text(json.dumps(record, indent=1), encoding="utf-8")
    scores = {}
    for lens in council.get("lenses", []):
        for variant, cats in lens.get("ratings", {}).items():
            for cat, val in cats.items():
                if val and val[0] is not None:
                    scores.setdefault(variant, []).append(val[0])
    index = {"id": review_id, "date": report["provenance"]["date"], "title": council.get("title"), "commit": report["provenance"]["commit"],
             "dirty": bool(report["provenance"]["dirty_files"]), "scenes": list(report["scenes"]), "variants": list(report["variants"]),
             "chosen": council.get("chosen"), "mean_scores": {k: round(sum(v) / len(v), 2) for k, v in scores.items()}}
    with open(REVIEWS / "index.jsonl", "a", encoding="utf-8") as f:
        f.write(json.dumps(index) + chr(10))
    saved = (target / "review.json").exists() and (target / "sheet.png").exists()
    print(f"review {review_id} saved={saved} -> {target}")
    return 0 if saved else 1


def cmd_history(args) -> int:
    path = REVIEWS / "index.jsonl"
    if not path.exists():
        print("no reviews yet")
        return 0
    for line in path.read_text(encoding="utf-8").splitlines():
        entry = json.loads(line)
        if args.scene and args.scene not in entry["scenes"]:
            continue
        print(f"{entry['id']}  {entry['title']}  chosen={entry['chosen']}  scores={entry['mean_scores']}  scenes={entry['scenes']}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    run = sub.add_parser("run", help="run an A/B spec")
    run.add_argument("spec")
    run.add_argument("--out")
    run.add_argument("--exe")
    run.add_argument("--resume", action="store_true", help="reuse variants already captured in --out (each has frame_ms.json)")
    run.add_argument("--project", help="scratch copy of an Earth-system project (world edits and the clock are saved on close)")
    run.set_defaults(func=cmd_run)
    council = sub.add_parser("council", help="persist a council review of a finished run")
    council.add_argument("out_dir")
    council.add_argument("findings")
    council.set_defaults(func=cmd_council)
    history = sub.add_parser("history", help="list saved council reviews (optionally those sharing a scene name)")
    history.add_argument("--scene")
    history.set_defaults(func=cmd_history)
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
