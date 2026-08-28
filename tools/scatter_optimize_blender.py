#!/usr/bin/env python3
"""Blender-side optimizer for Asterra ecological scatter source models.

Run through Blender, not normal Python:

    blender --background --factory-startup \
      --python tools/scatter_optimize_blender.py -- \
      --asset boulder_01

The script reads assets/scatter/source/<asset>/source.json, imports the downloaded
source glTF/GLB, normalizes the pivot to ground centre, preserves provider LODs
when they are identifiable, and only auto-decimates asset classes for which mesh
decimation is safe (rocks and non-foliage deadwood). It writes game-authoring
GLBs and metadata under assets/scatter/runtime/<asset>/.

Foliage is deliberately conservative: alpha-card vegetation and tree crowns are
never blindly decimated. If provider LOD meshes cannot be identified, only LOD0
is emitted and metadata records that additional LOD authoring is required.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import re
import sys
from typing import Any, Iterable

import bpy
from mathutils import Vector

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = REPO_ROOT / "assets" / "scatter" / "asset_manifest.json"
LOD_RE = re.compile(r"(?:^|[_ .-])lod[_ .-]?(\d+)(?:$|[_ .-])", re.IGNORECASE)

# These classes have continuous opaque geometry for which Blender's collapse
# decimator is predictable enough for an automated first pass.
SAFE_DECIMATE_KINDS = {
    "stone",
    "stone_set",
    "mossy_stone_set",
    "boulder",
    "desert_boulder",
    "coastal_rocks",
    "deadwood",
    "dry_deadwood",
    "stump",
    "root",
    "dry_debris",
}

# Ratio is relative to the imported high-detail mesh. LOD0 remains untouched.
AUTO_LOD_RATIOS = (1.0, 0.45, 0.16)


def read_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        value = json.load(handle)
    if not isinstance(value, dict):
        raise ValueError(f"Expected JSON object in {path}")
    return value


def find_asset(manifest: dict[str, Any], asset_id: str) -> dict[str, Any]:
    assets = manifest.get("assets", [])
    if not isinstance(assets, list):
        raise ValueError("Manifest assets must be an array")
    for raw in assets:
        if isinstance(raw, dict) and raw.get("id") == asset_id:
            return raw
    raise KeyError(f"Unknown scatter asset: {asset_id}")


def clear_scene() -> None:
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    for datablocks in (bpy.data.meshes, bpy.data.curves, bpy.data.cameras, bpy.data.lights):
        for datablock in list(datablocks):
            if datablock.users == 0:
                datablocks.remove(datablock)


def import_gltf(path: Path) -> None:
    suffix = path.suffix.lower()
    if suffix not in {".gltf", ".glb"}:
        raise RuntimeError(f"Unsupported source format {suffix}; expected glTF/GLB")
    result = bpy.ops.import_scene.gltf(filepath=str(path))
    if "FINISHED" not in result:
        raise RuntimeError(f"Blender failed to import {path}")


def mesh_objects() -> list[bpy.types.Object]:
    return [obj for obj in bpy.context.scene.objects if obj.type == "MESH" and obj.data is not None]


def object_triangle_count(obj: bpy.types.Object) -> int:
    mesh = obj.data
    mesh.calc_loop_triangles()
    return len(mesh.loop_triangles)


def triangle_count(objects: Iterable[bpy.types.Object]) -> int:
    return sum(object_triangle_count(obj) for obj in objects)


def world_bounds(objects: Iterable[bpy.types.Object]) -> tuple[Vector, Vector]:
    lower = Vector((math.inf, math.inf, math.inf))
    upper = Vector((-math.inf, -math.inf, -math.inf))
    found = False
    for obj in objects:
        for corner in obj.bound_box:
            point = obj.matrix_world @ Vector(corner)
            lower.x = min(lower.x, point.x)
            lower.y = min(lower.y, point.y)
            lower.z = min(lower.z, point.z)
            upper.x = max(upper.x, point.x)
            upper.y = max(upper.y, point.y)
            upper.z = max(upper.z, point.z)
            found = True
    if not found:
        raise RuntimeError("Imported asset contains no mesh bounds")
    return lower, upper


def normalize_ground_pivot(objects: list[bpy.types.Object]) -> Vector:
    lower, upper = world_bounds(objects)
    offset = Vector((-(lower.x + upper.x) * 0.5, -(lower.y + upper.y) * 0.5, -lower.z))
    for obj in objects:
        obj.location += offset
    bpy.context.view_layer.update()
    return offset


def lod_index_for_object(obj: bpy.types.Object) -> int | None:
    names = [obj.name, obj.data.name if obj.data is not None else ""]
    collection = obj.users_collection[0] if obj.users_collection else None
    if collection is not None:
        names.append(collection.name)
    parent = obj.parent
    while parent is not None:
        names.append(parent.name)
        parent = parent.parent
    for name in names:
        match = LOD_RE.search(name)
        if match:
            return int(match.group(1))
    return None


def partition_provider_lods(objects: list[bpy.types.Object]) -> tuple[dict[int, list[bpy.types.Object]], list[bpy.types.Object]]:
    groups: dict[int, list[bpy.types.Object]] = {}
    common: list[bpy.types.Object] = []
    for obj in objects:
        index = lod_index_for_object(obj)
        if index is None:
            common.append(obj)
        else:
            groups.setdefault(index, []).append(obj)
    return groups, common


def duplicate_objects(objects: Iterable[bpy.types.Object], suffix: str) -> list[bpy.types.Object]:
    result: list[bpy.types.Object] = []
    collection = bpy.context.scene.collection
    for source in objects:
        clone = source.copy()
        clone.data = source.data.copy()
        clone.name = f"{source.name}_{suffix}"
        collection.objects.link(clone)
        clone.matrix_world = source.matrix_world.copy()
        result.append(clone)
    return result


def delete_objects(objects: Iterable[bpy.types.Object]) -> None:
    for obj in list(objects):
        if obj.name in bpy.data.objects:
            bpy.data.objects.remove(obj, do_unlink=True)


def apply_decimate(obj: bpy.types.Object, ratio: float) -> None:
    if ratio >= 0.999 or object_triangle_count(obj) < 64:
        return
    modifier = obj.modifiers.new(name="AsterraAutoLOD", type="DECIMATE")
    modifier.decimate_type = "COLLAPSE"
    modifier.ratio = max(0.02, min(1.0, ratio))
    modifier.use_collapse_triangulate = True
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    result = bpy.ops.object.modifier_apply(modifier=modifier.name)
    if "FINISHED" not in result:
        raise RuntimeError(f"Failed to apply decimator to {obj.name}")


def select_only(objects: Iterable[bpy.types.Object]) -> None:
    bpy.ops.object.select_all(action="DESELECT")
    first: bpy.types.Object | None = None
    for obj in objects:
        obj.hide_set(False)
        obj.hide_render = False
        obj.select_set(True)
        if first is None:
            first = obj
    if first is not None:
        bpy.context.view_layer.objects.active = first


def export_glb(objects: list[bpy.types.Object], destination: Path) -> None:
    if not objects:
        raise RuntimeError(f"Refusing to export empty LOD to {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    select_only(objects)
    result = bpy.ops.export_scene.gltf(
        filepath=str(destination),
        export_format="GLB",
        use_selection=True,
    )
    if "FINISHED" not in result:
        raise RuntimeError(f"Blender failed to export {destination}")


def collision_ratio(kind: str) -> float | None:
    if kind in {"boulder", "desert_boulder", "coastal_rocks", "stone_set", "mossy_stone_set"}:
        return 0.06
    if kind in {"deadwood", "dry_deadwood", "stump", "root"}:
        return 0.10
    return None


def export_auto_collision(source_objects: list[bpy.types.Object], kind: str, destination: Path) -> dict[str, Any] | None:
    ratio = collision_ratio(kind)
    if ratio is None:
        return None
    clones = duplicate_objects(source_objects, "collision")
    try:
        for obj in clones:
            apply_decimate(obj, ratio)
        export_glb(clones, destination)
        return {
            "path": destination.name,
            "triangles": triangle_count(clones),
            "mode": "simplified_visual_hull_source",
        }
    finally:
        delete_objects(clones)


def export_provider_lods(
    groups: dict[int, list[bpy.types.Object]],
    common: list[bpy.types.Object],
    output_dir: Path,
) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    provider_indices = sorted(groups)
    # Limit the initial runtime contract to three mesh LODs. Extra provider levels
    # can be reconsidered once screen-size selection is profiled in Godot.
    for runtime_index, provider_index in enumerate(provider_indices[:3]):
        objects = list(common) + list(groups[provider_index])
        destination = output_dir / f"lod{runtime_index}.glb"
        export_glb(objects, destination)
        records.append({
            "lod": runtime_index,
            "path": destination.name,
            "triangles": triangle_count(objects),
            "source": "provider",
            "provider_lod": provider_index,
        })
    return records


def export_generated_lods(
    source_objects: list[bpy.types.Object],
    kind: str,
    output_dir: Path,
) -> tuple[list[dict[str, Any]], bool]:
    records: list[dict[str, Any]] = []
    lod0 = output_dir / "lod0.glb"
    export_glb(source_objects, lod0)
    records.append({"lod": 0, "path": lod0.name, "triangles": triangle_count(source_objects), "source": "source"})

    if kind not in SAFE_DECIMATE_KINDS:
        return records, True

    for lod_index, ratio in enumerate(AUTO_LOD_RATIOS[1:], start=1):
        clones = duplicate_objects(source_objects, f"lod{lod_index}")
        try:
            for obj in clones:
                apply_decimate(obj, ratio)
            destination = output_dir / f"lod{lod_index}.glb"
            export_glb(clones, destination)
            records.append({
                "lod": lod_index,
                "path": destination.name,
                "triangles": triangle_count(clones),
                "source": "auto_decimate",
                "ratio": ratio,
            })
        finally:
            delete_objects(clones)
    return records, False


def parse_args() -> argparse.Namespace:
    # Blender leaves its own command-line arguments in sys.argv. User arguments
    # conventionally follow a standalone `--`.
    user_args = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    parser = argparse.ArgumentParser()
    parser.add_argument("--asset", required=True)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--output-root", type=Path)
    return parser.parse_args(user_args)


def main() -> int:
    args = parse_args()
    manifest_path = args.manifest.resolve()
    manifest = read_json(manifest_path)
    asset = find_asset(manifest, args.asset)
    kind = str(asset.get("kind", ""))

    source_cache = REPO_ROOT / str(manifest.get("source_cache", "assets/scatter/source"))
    source_dir = source_cache / args.asset
    provenance_path = source_dir / "source.json"
    if not provenance_path.exists():
        raise FileNotFoundError(
            f"Missing {provenance_path}. Fetch this source asset before optimizing it."
        )
    provenance = read_json(provenance_path)
    root_file = provenance.get("root_file")
    if not isinstance(root_file, str) or not root_file:
        raise ValueError(f"{provenance_path} does not define root_file")
    source_path = source_dir / root_file
    if not source_path.exists():
        raise FileNotFoundError(source_path)

    if args.output_root is None:
        output_root = REPO_ROOT / str(manifest.get("runtime_root", "assets/scatter/runtime"))
    else:
        output_root = args.output_root.resolve()
    output_dir = output_root / args.asset
    output_dir.mkdir(parents=True, exist_ok=True)

    clear_scene()
    import_gltf(source_path)
    objects = mesh_objects()
    if not objects:
        raise RuntimeError(f"{args.asset} imported without mesh objects")

    pivot_offset = normalize_ground_pivot(objects)
    source_bounds = world_bounds(objects)
    source_triangles = triangle_count(objects)
    groups, common = partition_provider_lods(objects)

    if groups:
        lod_records = export_provider_lods(groups, common, output_dir)
        lod_authoring_required = len(lod_records) < 3
        collision_source = list(common) + list(groups[sorted(groups)[0]])
        lod_strategy = "provider"
    else:
        lod_records, lod_authoring_required = export_generated_lods(objects, kind, output_dir)
        collision_source = objects
        lod_strategy = "auto_safe_decimate" if kind in SAFE_DECIMATE_KINDS else "lod0_only_foliage_safe"

    collision = export_auto_collision(collision_source, kind, output_dir / "collision.glb")
    lower, upper = source_bounds
    size = upper - lower
    metadata = {
        "schema_version": 1,
        "asset_id": args.asset,
        "kind": kind,
        "source_provider": asset.get("provider", manifest.get("provider", "Poly Haven")),
        "source_url": asset.get("url"),
        "source_resolution": provenance.get("resolved_resolution", provenance.get("requested_resolution")),
        "source_triangles": source_triangles,
        "pivot_policy": "xy_bounds_center_z_ground",
        "pivot_offset_m": [pivot_offset.x, pivot_offset.y, pivot_offset.z],
        "bounds_size_m": [size.x, size.y, size.z],
        "lod_strategy": lod_strategy,
        "lod_authoring_required": lod_authoring_required,
        "lods": lod_records,
        "collision": collision,
        "biomes": asset.get("biomes", []),
        "priority": asset.get("priority", "secondary"),
    }
    with (output_dir / "metadata.json").open("w", encoding="utf-8") as handle:
        json.dump(metadata, handle, indent=2, sort_keys=True)
        handle.write("\n")

    print(json.dumps(metadata, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
