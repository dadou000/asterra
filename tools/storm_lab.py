"""Launches Studio on examples/storm-lab, places the storm, and leaves the scene running at night near the storm."""
import sys, pathlib, math, time
ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import orbit_ab as ab
spec = {"project": str(ROOT / "examples" / "storm-lab"), "world": "Worlds/Main.orbitworld", "body": "Earth", "fixed_exposure": False}
ab.launch(ROOT / "Orbit.exe", pathlib.Path(spec["project"]), {"ORBIT_CLOUD_POINT_LIGHTS": "1"})
radius = ab.prepare_session(spec)
scene = {"name": "night", "time_microseconds": 18070405751 + 26100 * 1000000, "local_time_seconds": 69300,
         "pose": {"kind": "surface", "lat_deg": 5.0, "lon_deg": 0.0, "alt_m": 42, "yaw_deg": 0, "pitch_deg": 12}}
ab.set_scene(scene, radius)
print(ab.rpc("storm.lightning", {"slow_motion": 1.0, "charge_boost": 1000.0, "breakdown_scale": 0.08, "brightness": 30.0, "arcs": True}))
ab.rpc("storm.place", {"latitude_deg": 5.55, "longitude_deg": 0.0, "spin_minutes": 80, "cells": 128, "nested": True})
st = {}
for _ in range(120):
    ab.wait_frames(10, timeout=10)
    st = ab.rpc("storm.status")
    if st.get("state") == "active" and not st.get("catching_up"):
        break
cam_lat = st["latitude_deg"] - math.degrees(14000.0 / 6371000.0)
ab.apply_pose({"kind": "surface", "lat_deg": cam_lat, "lon_deg": st["longitude_deg"], "alt_m": 42, "yaw_deg": 0, "pitch_deg": 14}, radius)
ab.rpc("time.set", {"playing": True, "rate": 1.0})
print("running; storm at", st["latitude_deg"], st["longitude_deg"], flush=True)
