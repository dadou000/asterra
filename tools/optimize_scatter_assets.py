#!/usr/bin/env python3
"""Batch wrapper for tools/scatter_optimize_blender.py.

Examples:
    python tools/optimize_scatter_assets.py --all --priority core
    python tools/optimize_scatter_assets.py --biome TAIGA
    python tools/optimize_scatter_assets.py --asset boulder_01
    python tools/optimize_scatter_assets.py --all --blender "C:/Program Files/Blender Foundation/Blender 4.5/blender.exe"
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = REPO_ROOT / "assets" / "scatter" / "asset_manifest.json"
BLENDER_SCRIPT = REPO_ROOT / "tools" / "scatter_optimize_blender.py"


def read_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        value = json.load(handle)
    if not isinstance(value, dict):
        raise ValueError(f"Expected JSON object in {path}")
    return value


def find_blender(explicit: str | None) -> str:
    if explicit:
        path = Path(explicit).expanduser()
        if path.exists():
            return str(path.resolve())
        found = shutil.which(explicit)
        if found:
            return found
        raise FileNotFoundError(f"Blender executable not found: {explicit}")

    found = shutil.which("blender")
    if found:
        return found

    # Common Windows installs. Newer versions first; this keeps the wrapper useful
    # on a development workstation without forcing Blender onto PATH.
    roots = [
        Path("C:/Program Files/Blender Foundation"),
        Path("C:/Program Files (x86)/Blender Foundation"),
    ]
    candidates: list[Path] = []
    for root in roots:
        if root.exists():
            candidates.extend(root.glob("Blender */blender.exe"))
    if candidates:
        return str(sorted(candidates, reverse=True)[0])

    raise FileNotFoundError("Blender not found. Pass --blender /path/to/blender")


def select_assets(
    manifest: dict[str, Any],
    ids: set[str],
    biome: str | None,
    priority: str | None,
    select_all: bool,
) -> list[dict[str, Any]]:
    assets = manifest.get("assets", [])
    if not isinstance(assets, list):
        raise ValueError("Manifest assets must be an array")
    result: list[dict[str, Any]] = []
    for raw in assets:
        if not isinstance(raw, dict):
            continue
        asset_id = raw.get("id")
        if not isinstance(asset_id, str):
            continue
        if ids and asset_id not in ids:
            continue
        if biome is not None:
            biomes = raw.get("biomes", [])
            if not isinstance(biomes, list) or biome not in biomes:
                continue
        if priority is not None and raw.get("priority") != priority:
            continue
        if select_all or ids or biome is not None:
            result.append(raw)
    return result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Optimize fetched scatter sources through Blender")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--asset", action="append", default=[], help="Asset slug; may be repeated")
    parser.add_argument("--biome", help="Exact manifest biome key")
    parser.add_argument("--priority", choices=("core", "secondary"))
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--blender", help="Path/name of Blender executable")
    parser.add_argument("--force", action="store_true", help="Rebuild even when runtime metadata exists")
    parser.add_argument("--keep-going", action="store_true", help="Continue after an asset fails")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    manifest_path = args.manifest.resolve()
    manifest = read_json(manifest_path)
    selected = select_assets(manifest, set(args.asset), args.biome, args.priority, args.all)
    if not selected:
        print("No assets selected. Use --asset, --biome or --all.", file=sys.stderr)
        return 2

    blender = find_blender(args.blender)
    source_root = REPO_ROOT / str(manifest.get("source_cache", "assets/scatter/source"))
    runtime_root = REPO_ROOT / str(manifest.get("runtime_root", "assets/scatter/runtime"))

    failures: list[tuple[str, str]] = []
    skipped = 0
    built = 0
    for index, asset in enumerate(selected, start=1):
        asset_id = str(asset["id"])
        source_meta = source_root / asset_id / "source.json"
        runtime_meta = runtime_root / asset_id / "metadata.json"
        print(f"[{index}/{len(selected)}] {asset_id}")

        if not source_meta.exists():
            message = "source not fetched"
            failures.append((asset_id, message))
            print(f"  missing: {source_meta}", file=sys.stderr)
            if not args.keep_going:
                break
            continue
        if runtime_meta.exists() and not args.force:
            print("  cached runtime metadata; use --force to rebuild")
            skipped += 1
            continue

        command = [
            blender,
            "--background",
            "--factory-startup",
            "--python",
            str(BLENDER_SCRIPT),
            "--",
            "--asset",
            asset_id,
            "--manifest",
            str(manifest_path),
        ]
        completed = subprocess.run(command, cwd=REPO_ROOT, check=False)
        if completed.returncode != 0:
            failures.append((asset_id, f"Blender exited {completed.returncode}"))
            if not args.keep_going:
                break
        else:
            built += 1

    print(f"\nbuilt={built} skipped={skipped} failed={len(failures)}")
    if failures:
        for asset_id, message in failures:
            print(f"  {asset_id}: {message}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
