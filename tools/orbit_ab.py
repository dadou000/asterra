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
    frame_ms = json.loads(meta.read_text(encoding="utf-8"))
    return {n: np.asarray(Image.open(out / label / f"{n}.png").convert("RGB"), dtype=np.float32) for n in names}, frame_ms


def run_variant(label: str, env_vars: dict, spec: dict, exe: pathlib.Path, out: pathlib.Path) -> tuple[dict[str, np.ndarray], dict[str, float]]:
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
                                        scene.get("min_wait", 3.0), scene.get("max_wait", 45.0), 2.0)
        images[scene["name"]] = image
        recent = rpc("profiler.status").get("recent_frame_ms") or []
        frame_ms[scene["name"]] = round(float(np.median(recent)), 2) if recent else 0.0
        print(f"  {label:10s} {scene['name']:18s} converged in {used:4.1f} s  frame {frame_ms[scene["name"]]:6.2f} ms", flush=True)
    (out / label / "frame_ms.json").write_text(json.dumps(frame_ms), encoding="utf-8")
    return images, frame_ms


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
    images: dict[str, dict] = {}
    frame: dict[str, dict] = {}
    started = time.time()
    try:
        for label in order:
            cached = load_variant(label, spec, out) if args.resume else None
            if cached:
                print(f"[{label}] reused from {out / label}", flush=True)
                images[label], frame[label] = cached
                continue
            print(f"[{label}] env={variants[label].get('env', {})}", flush=True)
            images[label], frame[label] = run_variant(label, variants[label].get("env", {}), spec, exe, out)
        controls = []
        for i in range(int(spec.get("controls", 1))):
            name = f"{baseline}_control{i + 1}"
            cached = load_variant(name, spec, out) if args.resume else None
            if cached:
                print(f"[{name}] reused", flush=True)
                images[name], frame[name] = cached
            else:
                print(f"[{name}] noise control", flush=True)
                images[name], frame[name] = run_variant(name, variants[baseline].get("env", {}), spec, exe, out)
            controls.append(name)
    finally:
        kill_orbit()
    threshold = float(spec.get("changed_pixel_threshold", 12))
    report = {"baseline": baseline, "seconds": round(time.time() - started, 1), "provenance": provenance(exe),
              "variants": {v: variants[v].get("env", {}) for v in order}, "spec": spec, "scenes": {}}
    for scene in spec["scenes"]:
        name, box = scene["name"], scene.get("box")
        entry = {"noise": [metrics(images[baseline][name], images[c][name], box, threshold) for c in controls]}
        entry["variants"] = {v: metrics(images[baseline][name], images[v][name], box, threshold) for v in order[1:]}
        entry["frame_ms"] = {v: frame[v][name] for v in order}
        # Launch-to-launch drift (GPU clocks, order): the same build measured again. Cost differences below it are not resolved.
        entry["frame_ms_drift"] = [round(frame[c][name] - frame[baseline][name], 2) for c in controls]
        entry["image_sha256"] = {v: sha256(out / v / f"{name}.png") for v in order}
        entry["resolution"] = list(images[baseline][name].shape[:2])
        report["scenes"][name] = entry
    (out / "report.json").write_text(json.dumps(report, indent=1), encoding="utf-8")
    make_sheet(out, order, [s["name"] for s in spec["scenes"]], images, baseline)
    print(f"\nbaseline={baseline}  total {report['seconds']} s  ->  {out}")
    for name, entry in report["scenes"].items():
        noise = max((n.get("mean", 0.0) for n in entry["noise"]), default=0.0)
        for v, m in entry["variants"].items():
            verdict = "DIFFERENT" if m.get("mean", 0.0) > max(3.0 * noise, noise + 0.05) else "same as noise"
            print(f"  {name:18s} {v:10s} mean {m.get('mean')}  max {m.get('max')}  px>{threshold:g}: {m.get('px_over')}  (noise mean {noise:.3f})  {verdict}  frame {entry['frame_ms'][v]} vs {entry['frame_ms'][baseline]} ms (drift of the same build: {entry['frame_ms_drift']})")
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
