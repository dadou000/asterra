#!/usr/bin/env python3
"""Validate scatter manifest structure and optionally verify Poly Haven slugs online."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = REPO_ROOT / "assets" / "scatter" / "asset_manifest.json"
API_ROOT = "https://api.polyhaven.com"
USER_AGENT = "AsterraScatterAuthoring/0.1 (Powered by Poly Haven)"
VALID_BIOMES = (
    "OCEAN",
    "SHELF_SEA",
    "ICE_CAP",
    "TUNDRA",
    "TAIGA",
    "COLD_DESERT",
    "TEMPERATE_GRASSLAND",
    "TEMPERATE_FOREST",
    "TEMPERATE_RAINFOREST",
    "MEDITERRANEAN",
    "STEPPE",
    "HOT_DESERT",
    "SAVANNA",
    "TROPICAL_SEASONAL_FOREST",
    "TROPICAL_RAINFOREST",
    "WETLAND",
    "ALPINE",
    "BARE_ROCK",
)
VALID_PRIORITIES = {"core", "secondary"}
VALID_RESOLUTIONS = {"1k", "2k", "4k", "8k", "16k"}


def load_manifest(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)
    if not isinstance(data, dict):
        raise ValueError("Manifest root must be an object")
    return data


def api_json(url: str) -> dict[str, Any]:
    request = Request(url, headers={"User-Agent": USER_AGENT, "Accept": "application/json"})
    try:
        with urlopen(request, timeout=45) as response:
            value = json.loads(response.read().decode("utf-8"))
    except (HTTPError, URLError) as exc:
        raise RuntimeError(str(exc)) from exc
    if not isinstance(value, dict):
        raise RuntimeError("API response was not an object")
    return value


def has_gltf(files: dict[str, Any]) -> bool:
    branch = files.get("gltf")
    if not isinstance(branch, dict):
        return False
    for resolution in branch.values():
        if not isinstance(resolution, dict):
            continue
        for value in resolution.values():
            if isinstance(value, dict) and isinstance(value.get("url"), str):
                url = str(value["url"]).lower()
                if url.endswith((".gltf", ".glb")):
                    return True
    return False


def validate(data: dict[str, Any], online: bool) -> list[str]:
    errors: list[str] = []
    plan = data.get("biome_plan")
    if not isinstance(plan, dict):
        errors.append("biome_plan must be an object")
        plan = {}
    for biome in VALID_BIOMES:
        if biome not in plan:
            errors.append(f"missing biome_plan entry: {biome}")

    assets = data.get("assets")
    if not isinstance(assets, list):
        return errors + ["assets must be an array"]

    allow_empty_catalog = data.get("allow_empty_catalog", False)
    if not isinstance(allow_empty_catalog, bool):
        errors.append("allow_empty_catalog must be a boolean when present")
        allow_empty_catalog = False
    if allow_empty_catalog and assets:
        errors.append("allow_empty_catalog=true requires assets to be an empty array")

    seen: set[str] = set()
    coverage = {biome: 0 for biome in VALID_BIOMES}
    for index, raw in enumerate(assets):
        if not isinstance(raw, dict):
            errors.append(f"assets[{index}] must be an object")
            continue
        asset_id = raw.get("id")
        if not isinstance(asset_id, str) or not asset_id:
            errors.append(f"assets[{index}] has invalid id")
            continue
        if asset_id in seen:
            errors.append(f"duplicate id: {asset_id}")
        seen.add(asset_id)

        kind = raw.get("kind")
        if not isinstance(kind, str) or not kind:
            errors.append(f"{asset_id}: missing kind")
        if raw.get("priority") not in VALID_PRIORITIES:
            errors.append(f"{asset_id}: invalid priority")
        if raw.get("resolution") not in VALID_RESOLUTIONS:
            errors.append(f"{asset_id}: invalid resolution")

        biomes = raw.get("biomes")
        if not isinstance(biomes, list) or not biomes:
            errors.append(f"{asset_id}: biomes must be a non-empty array")
        else:
            for biome in biomes:
                if biome not in coverage:
                    errors.append(f"{asset_id}: unknown biome {biome!r}")
                else:
                    coverage[biome] += 1

        expected_url = f"https://polyhaven.com/a/{asset_id}"
        if raw.get("url") != expected_url:
            errors.append(f"{asset_id}: expected source URL {expected_url}")

        if online:
            try:
                info = api_json(f"{API_ROOT}/info/{asset_id}")
                if int(info.get("type", -1)) != 2:
                    errors.append(f"{asset_id}: Poly Haven asset is not a model")
                files = api_json(f"{API_ROOT}/files/{asset_id}")
                if not has_gltf(files):
                    errors.append(f"{asset_id}: no glTF/GLB download exposed by API")
            except Exception as exc:
                errors.append(f"{asset_id}: online verification failed: {exc}")

    # Empty catalogs are permitted only when explicitly declared. Otherwise the
    # normal coverage invariant remains strict: every non-ocean macro biome needs
    # at least one concrete model assignment.
    if not allow_empty_catalog:
        for biome, count in coverage.items():
            if biome != "OCEAN" and count == 0:
                errors.append(f"no model coverage for biome: {biome}")

    print(f"assets: {len(assets)}")
    print(f"intentional_empty_catalog: {allow_empty_catalog}")
    print("coverage:")
    for biome in VALID_BIOMES:
        print(f"  {biome:28} {coverage[biome]:2d}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--online", action="store_true", help="Verify every slug/type/glTF through Poly Haven API")
    args = parser.parse_args()

    try:
        data = load_manifest(args.manifest.resolve())
        errors = validate(data, args.online)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    if errors:
        print("\nvalidation errors:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    print("\nscatter manifest valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
