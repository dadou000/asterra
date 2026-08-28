#!/usr/bin/env python3
"""Audit or remove local scatter assets that no longer satisfy the curated manifest.

Default mode is read-only. Pass --apply to delete:
  * source/runtime asset directories no longer present in the manifest;
  * runtime asset directories containing a file above the runtime file-size cap;
  * runtime assets whose metadata reports LOD0 above the triangle cap.

Raw source scans remain local and gitignored, so this is primarily for cleaning a
workstation after the curated asset list or quality budgets change.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[1]
MANIFEST_PATH = REPO_ROOT / "assets" / "scatter" / "asset_manifest.json"
MIB = 1024 * 1024


def read_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        value = json.load(handle)
    if not isinstance(value, dict):
        raise ValueError(f"Expected JSON object in {path}")
    return value


def policy_number(manifest: dict[str, Any], key: str, fallback: float) -> float:
    policy = manifest.get("selection_policy", {})
    if isinstance(policy, dict):
        try:
            return float(policy.get(key, fallback))
        except (TypeError, ValueError):
            pass
    return fallback


def runtime_violations(path: Path, manifest: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    max_file_mib = policy_number(manifest, "max_runtime_file_mib", 100.0)
    max_lod0 = int(policy_number(manifest, "max_runtime_lod0_triangles", 750000.0))
    max_bytes = int(max_file_mib * MIB)

    for child in path.rglob("*"):
        if child.is_file() and child.stat().st_size > max_bytes:
            errors.append(f"{child.name}={child.stat().st_size / MIB:.1f} MiB")

    metadata_path = path / "metadata.json"
    if metadata_path.exists():
        try:
            metadata = read_json(metadata_path)
            lods = metadata.get("lods", [])
            if isinstance(lods, list):
                for lod in lods:
                    if isinstance(lod, dict) and int(lod.get("lod", -1)) == 0:
                        tris = int(lod.get("triangles", 0) or 0)
                        if tris > max_lod0:
                            errors.append(f"LOD0={tris:,} tris")
                        break
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            errors.append(f"invalid metadata: {exc}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Audit/prune local Asterra scatter caches")
    parser.add_argument("--apply", action="store_true", help="Actually delete rejected directories")
    args = parser.parse_args()

    manifest = read_json(MANIFEST_PATH)
    asset_ids = {
        str(item["id"])
        for item in manifest.get("assets", [])
        if isinstance(item, dict) and isinstance(item.get("id"), str)
    }
    source_root = REPO_ROOT / str(manifest.get("source_cache", "assets/scatter/source"))
    runtime_root = REPO_ROOT / str(manifest.get("runtime_root", "assets/scatter/runtime"))

    removals: list[tuple[Path, str]] = []
    if source_root.exists():
        for child in source_root.iterdir():
            if child.is_dir() and child.name not in asset_ids:
                removals.append((child, "no longer in curated manifest"))

    if runtime_root.exists():
        for child in runtime_root.iterdir():
            if not child.is_dir():
                continue
            if child.name not in asset_ids:
                removals.append((child, "no longer in curated manifest"))
                continue
            errors = runtime_violations(child, manifest)
            if errors:
                removals.append((child, "; ".join(errors)))

    if not removals:
        print("Scatter cache is clean under the current manifest and budgets.")
        return 0

    for path, reason in removals:
        rel = path.relative_to(REPO_ROOT)
        action = "DELETE" if args.apply else "WOULD DELETE"
        print(f"{action:12} {rel}  ({reason})")
        if args.apply:
            shutil.rmtree(path)

    if not args.apply:
        print("\nRead-only audit. Re-run with --apply to remove these directories.")
    else:
        print(f"\nRemoved {len(removals)} rejected scatter asset directorie(s).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
