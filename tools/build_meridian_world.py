#!/usr/bin/env python3
"""Authors the Meridian showcase world (examples/meridian) through Studio's loopback JSON-RPC.

    python tools/build_meridian_world.py [--root DIR] [--exe Orbit.exe]

Creates a fresh project at DIR (default examples/meridian, must not exist), launches Studio on it, authors the world with the
same RPC methods the MCP tools wrap, saves and closes Studio. Nothing simulates input. The world uses the authored
(non-simulated) feature set: celestial bodies and capabilities, physical atmosphere, procedural cloud layers and genera,
ocean, rings, aurora, comet, compact object, terrain tectonics/hydrology/rivers/impacts/stratigraphy/processes/bake,
biomes, authored constraints, water, primitives (opaque/glass/mirror/emissive), lights, paths, visibility proxies, volumes.
It deliberately omits the CM1-derived storm domain and the lightning simulator.
"""
from __future__ import annotations

import argparse
import math
import pathlib
import shutil
import sys
import time
import traceback

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from meridian_lib import add_capability, child_of_type, children, create, prop_id, rpc, set_props, type_id  # noqa: E402
import orbit_ab as ab  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[1]
from meridian_lib import failures, step, recipe, vec, AU  # noqa: E402
from meridian_site import author_geology, author_site  # noqa: E402


def build(root: pathlib.Path, exe: pathlib.Path) -> None:
    ab.kill_orbit()
    if root.exists():
        raise SystemExit(f"{root} already exists")
    # Studio needs a project to start; start on a throwaway one and let project.create hand off to the real one.
    scratch = root.parent / (root.name + ".bootstrap")
    shutil.copytree(ROOT / "examples" / "mcp-smoke", scratch)
    ab.launch(exe, scratch, {})
    created = rpc("project.create", {"root": str(root).replace("\\", "/"), "name": "Meridian"})
    for _ in range(80):
        time.sleep(3)
        try:
            if rpc("project.info", timeout=3)["id"] == created["id"]:
                break
        except Exception:
            pass
    else:
        raise SystemExit("Studio did not switch to the new project")

    world = create("world", "Meridian World")
    system = create("system", "Meridian System", world)

    # ---- bodies -------------------------------------------------------------------------------------------------
    star = recipe("star", system, "Meridian Star", seed=3)
    planet = recipe("rocky_planet", system, "Meridian", radius_m=6.62e6, mass_kg=6.9e24, rotation_period_s=93600.0,
                    semi_major_axis_m=1.08 * AU, eccentricity=0.021, inclination_deg=0.8, atmosphere=True, ocean=True, seed=7)
    selene = recipe("moon", planet, "Selene", seed=11)
    ember = recipe("moon", planet, "Ember", radius_m=140e3, mass_kg=2.1e18, semi_major_axis_m=2.1e8, eccentricity=0.04,
                   inclination_deg=9.0, atmosphere=False, ocean=False, synchronous_rotation=False, seed=21)
    print("bodies", star, planet, selene, ember, flush=True)

    # ---- Meridian: atmosphere, clouds, ocean, aurora --------------------------------------------------------------
    @step("Meridian atmosphere preset + solve")
    def _():
        atm = child_of_type(planet, "Atmosphere")
        set_props(atm, "Atmosphere", {"Surface Pressure": 98000.0, "Surface Temperature": 287.0, "Aerosol Optical Depth 550 nm": 0.07})

    @step("Meridian cloud layers")
    def _():
        low = child_of_type(planet, "Cloud Layer")
        set_props(low, "Cloud Layer", {"Base Altitude": 900.0, "Top Altitude": 4200.0, "Coverage Bias": 0.05, "Peak Optical Depth": 14.0,
                                       "Weather Scale": 4.2, "Seed": 7, "Precipitation Phase": "Rain"})
        high = create("Cloud Layer", "Cirrus Layer", planet)
        set_props(high, "Cloud Layer", {"Base Altitude": 8500.0, "Top Altitude": 11500.0, "Coverage Bias": -0.2, "Peak Optical Depth": 1.6,
                                        "Phase Anisotropy": 0.82, "Weather Scale": 2.0, "Detail Scale": 30.0, "Seed": 19,
                                        "Precipitation Phase": "Ice", "Cast Cloud Shadows": False})

    @step("Meridian ocean look")
    def _():
        set_props(child_of_type(planet, "Ocean"), "Ocean", {"Deep Water Color": [0.004, 0.03, 0.08], "Sun Glint Strength": 1.3})

    @step("Meridian aurora")
    def _():
        add_capability(planet, "Magnetosphere / Aurora", {"Activity": 0.55, "Auroral Oval Latitude": 66.0, "Auroral Intensity": 1.2, "Auroral Seed": 5})

    # ---- Meridian terrain ----------------------------------------------------------------------------------------
    terrain = child_of_type(planet, "Terrain Surface")
    settings = child_of_type(terrain, "Terrain Process Settings") if terrain else None

    @step("terrain: tectonics")
    def _():
        rpc("terrain.tectonics_set", {"terrain": terrain, "seed": 7, "plate_count": 16, "plate_irregularity": 0.42, "continental_fraction": 0.36,
                                      "hotspot_count": 4, "hotspot_relief_meters": 5200.0, "belt_ridge_relief": 1.2})

    @step("terrain: hydrology")
    def _():
        rpc("terrain.hydrology_set", {"terrain": terrain, "rainfall_meters_per_second": 0.00025, "seasonal_rainfall_amplitude": 0.3})

    @step("terrain: rivers")
    def _():
        rpc("terrain.rivers_set", {"terrain": terrain, "enabled": True, "meanders": True, "cutoffs": True,
                                   "minimum_drainage_area_square_meters": 40000.0, "maximum_channel_width_meters": 220.0})

    @step("terrain: processes + erosion coupling")
    def _():
        rpc("terrain.processes_set", {"terrain": terrain, "hydraulic_enabled": True, "thermal_enabled": True, "aeolian_enabled": True,
                                      "glacial_enabled": True, "coastal_enabled": True, "stream_power_enabled": True})
        rpc("terrain.erosion_coupling_set", {"terrain": terrain, "age_erodibility_gain": 0.8, "tectonic_drainage_guidance": 0.6})

    @step("terrain: surface relief")
    def _():
        set_props(terrain, "Terrain Surface", {"Seed": 7070707, "Macro Amplitude": 1500.0, "Detail Amplitude": 380.0,
                                               "Maximum Elevation Above Sea Level": 9200.0})

    # ---- Selene / Ember ------------------------------------------------------------------------------------------
    @step("Ember small body")
    def _():
        add_capability(ember, "Small Body Appearance", {"Body Class": "Asteroid", "Seed": 21, "Axis Scale": [1.0, 0.8, 0.7], "Irregularity": 0.3})

    @step("Selene regolith")
    def _():
        add_capability(selene, "Small Body Appearance", {"Body Class": "Moonlet", "Seed": 1969, "Axis Scale": [1, 1, 1], "Irregularity": 0.004,
                                                         "Large Lobe Strength": 0.0, "Crater Density": 1.0, "Crater Depth": 0.03, "Crater Rim Strength": 0.015})

    # ---- Halcyon: gas giant with rings, aurora and an ice moon ---------------------------------------------------
    halcyon = create("body", "Halcyon", system)

    @step("Halcyon body")
    def _():
        set_props(halcyon, "Celestial Body", {"Equatorial / Reference Radius": 6.4e7, "Polar Radius": 5.9e7, "Ellipsoid Shape": True,
                                              "Mass": 7.4e27, "Rotation Period": 36000.0, "Axial Tilt": 21.0,
                                              "Parent-frame Position": [5.2 * AU, 0.0, 0.0]})
        add_capability(halcyon, "Gravity", {"Derive Mu From Body Mass": True})
        add_capability(halcyon, "Rotation / Orientation", {"Model": "Uniform Spin", "Rotation Period": 36000.0})
        add_capability(halcyon, "Orbit / Ephemeris", {"Model": "Analytic Conic", "Semi-major Axis Magnitude": 5.2 * AU, "Eccentricity": 0.048,
                                                      "Inclination": 1.3, "Gravitational Parameter": 1.32712440018e20, "Mean Anomaly At Epoch": 200.0})
        add_capability(halcyon, "Giant Appearance", {"Giant Class": "Gas Giant", "Seed": 33, "Band Frequency": 14.0, "Storm Strength": 0.5})
        add_capability(halcyon, "Magnetosphere / Aurora", {"Equatorial Field": 4.2e-4, "Activity": 0.7, "Auroral Oval Latitude": 72.0})

    @step("Halcyon rings")
    def _():
        rings = add_capability(halcyon, "Ring System", {"Ring Plane Normal": [0.0, 1.0, 0.0]})
        for name, (a, b, tau, albedo, col) in {
            "C Band": (8.0e7, 9.6e7, 0.12, 0.35, [0.45, 0.4, 0.35]),
            "B Band": (9.8e7, 1.3e8, 1.4, 0.7, [0.78, 0.7, 0.58]),
            "A Band": (1.36e8, 1.62e8, 0.55, 0.6, [0.7, 0.64, 0.55]),
        }.items():
            band = create("Ring Band", name, rings)
            set_props(band, "Ring Band", {"Inner Radius": a, "Outer Radius": b, "Normal Optical Depth": tau,
                                          "Single Scattering Albedo": albedo, "Ring Color": col})

    @step("Aegis ice moon")
    def _():
        aegis = recipe("moon", halcyon, "Aegis", radius_m=2.6e6, mass_kg=1.5e23, semi_major_axis_m=6.7e8, eccentricity=0.01,
                       atmosphere=False, ocean=False, seed=44)
        add_capability(aegis, "Small Body Appearance", {"Body Class": "Moonlet", "Regolith Color": [0.62, 0.66, 0.7], "Fresh Material Color": [0.8, 0.84, 0.88],
                                                        "Seed": 44, "Axis Scale": [1, 1, 1], "Irregularity": 0.004, "Crater Density": 0.5})

    # ---- Wanderer comet and Umbra compact object -----------------------------------------------------------------
    @step("Wanderer comet")
    def _():
        comet = create("body", "Wanderer", system)
        set_props(comet, "Celestial Body", {"Equatorial / Reference Radius": 6500.0, "Polar Radius": 6500.0, "Mass": 4.0e14,
                                            "Rotation Period": 41000.0, "Parent-frame Position": [1.6 * AU, 0.0, 0.0]})
        add_capability(comet, "Gravity", {"Derive Mu From Body Mass": True})
        add_capability(comet, "Rotation / Orientation", {"Rotation Period": 41000.0})
        add_capability(comet, "Orbit / Ephemeris", {"Model": "Analytic Conic", "Semi-major Axis Magnitude": 14.0 * AU, "Eccentricity": 0.93,
                                                    "Inclination": 38.0, "Gravitational Parameter": 1.32712440018e20, "Mean Anomaly At Epoch": 354.0})
        add_capability(comet, "Small Body Appearance", {"Body Class": "Comet Nucleus", "Seed": 77, "Irregularity": 0.35, "Regolith Color": [0.05, 0.048, 0.045]})
        add_capability(comet, "Comet Tail", {})

    @step("Umbra compact object")
    def _():
        umbra = create("body", "Umbra", system)
        mu = 6.674e-11 * 8.0 * 1.989e30
        set_props(umbra, "Celestial Body", {"Equatorial / Reference Radius": 2.4e4, "Polar Radius": 2.4e4, "Mass": 8.0 * 1.989e30,
                                            "Parent-frame Position": [60.0 * AU, 0.0, 0.0]})
        add_capability(umbra, "Gravity", {"Derive Mu From Body Mass": True})
        add_capability(umbra, "Compact Object", {"Gravitational Parameter": mu, "Dimensionless Spin": 0.0})
        add_capability(umbra, "Accretion Flow", {"Inner Radius": 6.0, "Outer Radius": 30.0})
        add_capability(umbra, "Orbit / Ephemeris", {"Model": "Analytic Conic", "Semi-major Axis Magnitude": 60.0 * AU, "Eccentricity": 0.1,
                                                    "Gravitational Parameter": 1.32712440018e20, "Mean Anomaly At Epoch": 80.0})

    # ---- terrain bake ----------------------------------------------------------------------------------------------
    @step("terrain: start bake and wait")
    def _():
        rpc("terrain.bake_start", {"terrain": terrain})
        for _ in range(240):
            st = rpc("terrain.bake_status", {"terrain": terrain})
            if st["state"] in ("ready", "failed"):
                if st["state"] == "failed":
                    raise RuntimeError(st.get("error"))
                return
            time.sleep(2.0)
        raise RuntimeError("bake did not finish in 8 minutes")

    author_geology(planet, terrain, selene)
    author_site(planet, terrain, 6.62e6)
    print("failures:", failures, flush=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(ROOT / "examples" / "meridian"))
    ap.add_argument("--exe", default=str(ROOT / "Orbit.exe"))
    ap.add_argument("--live", action="store_true", help="author only the geology and site sections into the running Studio")
    args = ap.parse_args()
    if args.live:
        w = rpc("object.roots")[0]["id"]
        sysid = children(w)[0]["id"]
        planet = next(k["id"] for k in children(sysid) if k["name"] == "Meridian")
        terrain = child_of_type(planet, "Terrain Surface")
        selene = next(k["id"] for k in children(planet) if k["name"] == "Selene")
        author_geology(planet, terrain, selene)
        author_site(planet, terrain, 6.62e6)
    else:
        build(pathlib.Path(args.root), pathlib.Path(args.exe))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
