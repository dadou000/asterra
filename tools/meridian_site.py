"""Geology and surface-site authoring for the Meridian showcase world (see build_meridian_world.py)."""
from __future__ import annotations

import math
import pathlib

import numpy as np

from meridian_lib import ab, child_of_type, children, create, failures, planet_id_for_body, prop_id, rpc, set_props, step, type_id, vec

ROOT = pathlib.Path(__file__).resolve().parents[1]
site: dict = {}


def euler_for_up(up: np.ndarray, heading_deg: float = 0.0) -> list[float]:
    """Primitive Euler degrees (R = Rz(c) Ry(b) Rx(a)) that put the local +Y on `up`, +Z toward a heading from north."""
    north, east = ab.surface_frame(up)
    h = math.radians(heading_deg)
    z = north * math.cos(h) + east * math.sin(h)
    x = np.cross(up, z)
    r = np.stack([x, up, z], axis=1)
    b = -math.asin(max(-1.0, min(1.0, r[2][0])))
    return [math.degrees(math.atan2(r[2][1], r[2][2])), math.degrees(b), math.degrees(math.atan2(r[1][0], r[0][0]))]


def author_geology(planet: str, terrain: str, selene: str) -> None:
    @step("terrain: stratigraphy")
    def _():
        toml = (ROOT / "engine/terrain_geology/assets/reference/continental_shelf.orbitstratigraphy").read_text(encoding="utf-8")
        rpc("terrain.stratigraphy_set", {"terrain": terrain, "profile_toml": toml})

    @step("terrain: impact history (wet)")
    def _():
        toml = f"""id = "4f524249-5449-4d50-4143-545300000010"
planet = "{planet_id_for_body(planet)}"
name = "Meridian impact history"
seed = "7"
environment = "wet"
surface_age_years = 4.5e9

[procedural]
count = 24
minimum_radius_m = 6000.0
maximum_radius_m = 90000.0
cumulative_exponent = 2.0
"""
        rpc("terrain.impacts_set", {"terrain": terrain, "history_toml": toml})

    @step("terrain: Selene airless impact history")
    def _():
        selene_terrain = child_of_type(selene, "Terrain Surface")
        toml = f"""id = "4f524249-5449-4d50-4143-545300000020"
planet = "{planet_id_for_body(selene)}"
name = "Selene impact history"
seed = "11"
environment = "airless"
surface_age_years = 4.45e9
surface_gravity_mps2 = 1.62

[procedural]
count = 400
minimum_radius_m = 800.0
maximum_radius_m = 120000.0
cumulative_exponent = 2.2

[[impact]]
id = "4f524249-5449-4d50-4143-545300000021"
radius_m = 320000.0
center_x = 0.6
center_y = 0.1
center_z = 0.79
formation_age_years = 3.9e9
multiring_strength = 0.8
ray_extent_radii = 6.0
"""
        rpc("terrain.impacts_set", {"terrain": selene_terrain, "history_toml": toml})

    @step("terrain: authored constraint + river basin constraint")
    def _():
        c = create("Authored Terrain Constraint", "Highland Plateau", terrain)
        set_props(c, "Authored Terrain Constraint", {"Center Unit Direction": list(vec(34.0, 9.0)), "Inner Radius": 30000.0,
                                                    "Outer Radius": 90000.0, "Value": 600.0, "Opacity": 0.8})
        r = create("River Basin Constraint", "Great Basin Guide", terrain)
        set_props(r, "River Basin Constraint", {"Influence Radius": 40000.0, "Strength": 0.7})


def author_site(planet: str, terrain: str, radius: float) -> None:
    """A showcase plaza on land: material primitives, lights, proxies, a path network and volumes."""

    @step("site: pick a land location")
    def _():
        pts = [{"latitude_degrees": float(la), "longitude_degrees": float(lo)}
               for la in np.arange(20.0, 41.0, 1.0) for lo in np.arange(-10.0, 11.0, 1.0)]
        samples = rpc("terrain.sample", {"terrain": terrain, "points": pts})["samples"]
        ok = sorted(((s["height_above_sea_level_meters"], i) for i, s in enumerate(samples)
                     if 30.0 < s["height_above_sea_level_meters"] < 400.0), key=lambda t: t[0])
        if not ok:
            raise RuntimeError("no lowland within the search window")
        p = pts[ok[len(ok) // 3][1]]
        site["lat"], site["lon"] = p["latitude_degrees"], p["longitude_degrees"]
        print("site", site, flush=True)

    if "lat" not in site:
        return
    lat, lon = site["lat"], site["lon"]
    north, east = ab.surface_frame(np.array(vec(lat, lon)))

    def at(e: float, n: float, up_m: float):
        la = lat + math.degrees(n / radius)
        lo = lon + math.degrees(e / (radius * math.cos(math.radians(lat))))
        h = rpc("terrain.sample", {"terrain": terrain, "points": [{"latitude_degrees": la, "longitude_degrees": lo}]})["samples"][0]["elevation_meters"]
        u = np.array(vec(la, lo))
        return (u * (radius + h + up_m)).tolist(), u, la, lo

    kit: dict = {}

    @step("site: primitives")
    def _():
        specs = [
            ("Plinth Stone", 0, 0, 0.5, "box", [3.0, 1.0, 3.0], dict(color=[0.55, 0.52, 0.48], roughness=0.85)),
            ("Mirror Monolith", -6, 2, 2.0, "box", [0.3, 4.0, 2.2], dict(surface="mirror")),
            ("Glass Orb", 4, 3, 1.4, "sphere", [2.4, 2.4, 2.4], dict(surface="glass", color=[0.85, 0.97, 1.0], ior=1.5)),
            ("Amber Glass Column", 8, -2, 2.0, "cylinder", [1.4, 4.0, 1.4], dict(surface="glass", color=[1.0, 0.7, 0.3], ior=1.45)),
            ("Brushed Steel Sphere", -3, -5, 1.0, "sphere", [2.0, 2.0, 2.0], dict(color=[0.8, 0.82, 0.85], roughness=0.22, metallic=1.0)),
            ("Ember Emitter", 2, -8, 1.2, "sphere", [1.2, 1.2, 1.2], dict(color=[1.0, 0.4, 0.1], emission_nits=60000.0)),
            ("Rough Capsule", 10, 6, 1.5, "capsule", [1.0, 3.0, 1.0], dict(color=[0.3, 0.55, 0.3], roughness=1.0)),
            ("Slab", -9, -8, 0.15, "plane", [6.0, 1.0, 4.0], dict(color=[0.25, 0.25, 0.3], roughness=0.35)),
        ]
        for i, (name, e, n, h, shape, size, kw) in enumerate(specs):
            pos, u, _la, _lo = at(e, n, h)
            euler = euler_for_up(u, 17.0 * i)
            r = rpc("primitive.create", {"parent": planet, "position": pos, "shape": shape, "size": size, "name": name,
                                         "euler_degrees": euler, **kw})
            kit[name] = (r["id"], pos, euler)

    @step("site: visibility proxies")
    def _():
        for name, shape in (("Mirror Monolith", 1), ("Glass Orb", 0)):
            _oid, pos, euler = kit[name]
            proxy = create("Visibility Proxy", name + " Proxy", planet)
            vals = {"Shape": shape, "Body-local Position": pos, "Maximum Approximation Error": 0.05}
            if shape == 0:
                vals["Sphere Radius"] = 1.2
            else:
                vals["Box Half Extents"] = [0.15, 2.0, 1.1]
                vals["Body-local Euler Rotation"] = euler
            set_props(proxy, "Visibility Proxy", vals)

    @step("site: lights")
    def _():
        pos, *_r = at(-2.0, 4.0, 5.0)
        set_props(create("Point Light", "Warm Lantern", planet), "Point Light", {"Position": pos, "Color": [1.0, 0.62, 0.3], "Luminous Flux": 4000.0, "Range": 30.0})
        pos, *_r = at(7.0, -6.0, 4.0)
        set_props(create("Point Light", "Cool Beacon", planet), "Point Light", {"Position": pos, "Color": [0.4, 0.65, 1.0], "Luminous Flux": 3000.0, "Range": 26.0})
        pos, u, *_r = at(0.0, -12.0, 8.0)
        direction = (-u * 0.8 + north * 0.6)
        set_props(create("Spot Light", "Plinth Spot", planet), "Spot Light",
                  {"Position": pos, "Direction": (direction / np.linalg.norm(direction)).tolist(), "Luminous Flux": 9000.0, "Range": 40.0,
                   "Inner Cone": 14.0, "Outer Cone": 28.0, "Color": [1.0, 0.95, 0.85]})

    @step("site: path network")
    def _():
        net = rpc("path.create_network", {"name": "Plaza Paths", "parent": planet})["id"]
        nodes = []
        for i, (e, n) in enumerate([(-20, -20), (-8, -6), (6, -4), (20, 10)]):
            *_r, la, lo = at(e, n, 0.0)
            nodes.append(rpc("path.create_node", {"network": net, "name": f"Waypoint {i + 1}",
                                                  "anchor": {"kind": "surface", "body": planet, "coordinate": [math.radians(la), math.radians(lo), 0.0]}})["id"])
        for a, b in zip(nodes, nodes[1:]):
            rpc("path.connect", {"start": a, "end": b, "mode": "direct"})

    @step("site: volumes (fog, smoke, fire)")
    def _():
        create_volume = next(c["id"] for c in rpc("command.catalog") if c["name"] == "Create Volume")
        rpc("selection.set", {"ids": [planet]})
        for preset in ("Fog", "Smoke", "Fire"):
            rpc("command.invoke", {"id": create_volume, "arguments": {"preset": preset}})
        vols = [k["id"] for k in children(planet) if k["type"] == type_id("Volume")]
        for vid, (e, n) in zip(vols, ((0, 0), (11, -3), (2, -8))):
            pos, *_r = at(e, n, 6.0)
            set_props(vid, "Volume", {"Center": pos})
