"""Small authoring helpers shared by tools/build_meridian_world.py: property lookup by name, object creation, edits."""
from __future__ import annotations

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import orbit_ab as ab  # noqa: E402

rpc = ab.rpc

TYPE = {
    "world": "4f524249-5457-4f52-4c44-545950450001",
    "system": "4f524249-5453-5953-5445-4d5459500001",
    "body": "4f524249-5442-4f44-5954-595045000001",
}

_catalog: dict[str, dict] | None = None


def catalog() -> dict[str, dict]:
    global _catalog
    if _catalog is None:
        _catalog = {c["name"]: c for c in rpc("schema.catalog")}
    return _catalog


def type_id(name: str) -> str:
    return catalog()[name]["id"]


def prop_id(type_name: str, prop_name: str) -> str:
    for p in catalog()[type_name]["properties"]:
        if p["name"] == prop_name:
            return p["id"]
    raise KeyError(f"{type_name!r} has no property {prop_name!r}")


def create(type_name_or_id: str, name: str, parent: str | None = None) -> str:
    tid = TYPE.get(type_name_or_id) or (catalog()[type_name_or_id]["id"] if type_name_or_id in catalog() else type_name_or_id)
    params = {"type": tid, "name": name}
    if parent:
        params["parent"] = parent
    r = rpc("object.create", params)
    return r.get("id") or r.get("object_id")


def set_props(object_id: str, type_name: str, values: dict) -> None:
    """Sets properties by display name, inside one transaction."""
    rpc("transaction.begin", {"label": f"Author {type_name}"})
    try:
        for key, value in values.items():
            rpc("property.set", {"object": object_id, "property": prop_id(type_name, key), "value": value})
    except Exception:
        rpc("transaction.rollback")
        raise
    rpc("transaction.commit")


def children(parent: str) -> list[dict]:
    return rpc("object.children", {"parent": parent})


def child_of_type(parent: str, type_name: str) -> str | None:
    tid = type_id(type_name)
    for kid in children(parent):
        if kid["type"] == tid:
            return kid["id"]
    return None


def add_capability(body: str, type_name: str, values: dict | None = None, name: str | None = None) -> str:
    oid = child_of_type(body, type_name) or create(type_name, name or type_name, body)
    if values:
        set_props(oid, type_name, values)
    return oid


import math  # noqa: E402
import time  # noqa: E402
import traceback  # noqa: E402

AU = 1.495978707e11
failures: list[str] = []


def step(label):
    def deco(fn):
        t0 = time.time()
        try:
            out = fn()
            print(f"[ok]   {label} ({time.time() - t0:.1f}s)", flush=True)
            return out
        except Exception as exc:  # keep going: report everything that failed at the end
            failures.append(f"{label}: {exc}")
            print(f"[FAIL] {label}: {exc}", flush=True)
            traceback.print_exc(limit=2)
            return None
    return deco


def recipe(kind, parent, name, **kw):
    return rpc("celestial.create_from_recipe", {"kind": kind, "parent": parent, "name": name, **kw})["id"]


def vec(lat_deg, lon_deg):
    la, lo = math.radians(lat_deg), math.radians(lon_deg)
    return (math.cos(la) * math.cos(lo), math.sin(la), -math.cos(la) * math.sin(lo))




def planet_id_for_body(body_object_id: str) -> str:
    """The PlanetId Studio derives from a body object (UniverseComposition DerivedId XOR salts), as an Orbit id string."""
    h = int(body_object_id.replace("-", ""), 16)
    high, low = h >> 64, h & ((1 << 64) - 1)
    high ^= 0x424F445949440003
    low ^= 0x4F52424954563033
    t = f"{high:016x}{low:016x}"
    return f"{t[0:8]}-{t[8:12]}-{t[12:16]}-{t[16:20]}-{t[20:32]}"
