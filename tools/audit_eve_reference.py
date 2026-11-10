#!/usr/bin/env python3
"""Audit a user-owned EVE V5 / Scatterer release without extracting assets.

This tool is intentionally independent of private source and copyrighted
payload. It emits *metadata and reference paths*, never shader binaries,
textures, audio, or complete proprietary configuration text.

Usage:
    python tools/audit_eve_reference.py "path/to/RaymarchedVolumetrics*.zip"
    python tools/audit_eve_reference.py "path/to/release.zip" --json
    python tools/audit_eve_reference.py "path/to/release.zip" --strict-manifest
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass, field
import hashlib
import json
from pathlib import Path
import sys
import zipfile


@dataclass
class Node:
    tag: str
    values: list[tuple[str, str]] = field(default_factory=list)
    children: list["Node"] = field(default_factory=list)

    def get(self, key: str, default: str = "") -> str:
        return next((v for k, v in self.values if k == key), default)

    def named(self, tag: str) -> list["Node"]:
        return [n for n in self.children if n.tag == tag]


def parse_nodes(text: str, label: str) -> list[Node]:
    """Preserve order, duplicate Item nodes, and repeated Hermite key lines."""
    roots: list[Node] = []
    stack: list[Node] = []
    pending: str | None = None
    for line_no, line in enumerate(text.splitlines(), 1):
        value = line.strip()
        if not value or value.startswith("//") or value.startswith("#"):
            continue
        if value == "{":
            if not pending:
                raise ValueError(f"{label}:{line_no}: unnamed block")
            node = Node(pending)
            (stack[-1].children if stack else roots).append(node)
            stack.append(node)
            pending = None
        elif value == "}":
            if pending is not None or not stack:
                raise ValueError(f"{label}:{line_no}: unexpected close")
            stack.pop()
        elif "=" in value:
            if pending is not None or not stack:
                raise ValueError(f"{label}:{line_no}: misplaced assignment")
            key, text_value = value.split("=", 1)
            stack[-1].values.append((key.strip(), text_value.strip()))
        else:
            if pending is not None:
                raise ValueError(f"{label}:{line_no}: missing opening brace")
            pending = value
    if stack or pending:
        raise ValueError(f"{label}: unclosed block")
    return roots


def fingerprint(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def layers_from_nodes(roots: list[Node]) -> list[dict]:
    out = []
    for root in roots:
        if root.tag != "EVE_CLOUDS":
            continue
        for obj in root.named("OBJECT"):
            volumes = obj.named("layerRaymarchedVolumeV5")
            for volume in volumes:
                groups = volume.named("cloudTypes")
                types = groups[0].named("Item") if groups else []
                painter = volume.named("painterTiles")
                type_names = [t.get("typeName") for t in types]
                out.append({
                    "planet": obj.get("body"),
                    "name": obj.get("name"),
                    "types": type_names,
                    "type_count": len(types),
                    "painter_tile_count": (
                        len(painter[0].named("Item")) if painter else 0
                    ),
                    "fade_start_m": volume.get("scaledFadeStartAltitude"),
                    "fade_end_m": volume.get("scaledFadeEndAltitude"),
                    "fx_only": (
                        volume.named("raymarchingSettings")[0]
                        .get("fxOnlyLayer", "False")
                        if volume.named("raymarchingSettings") else "False"
                    ),
                    "curve_key_count": sum(
                        len(curve.values)
                        for t in types for name in ("coverageCurve", "densityCurve")
                        for curve in t.named(name)
                        for _ in (0,)
                    ),
                })
    return out


def collect_paths(files: dict[str, bytes]) -> dict[str, list[str]]:
    refs: dict[str, list[str]] = defaultdict(list)
    for filename, data in files.items():
        if not filename.endswith(".cfg"):
            continue
        for line_no, line in enumerate(
            data.decode("utf-8-sig", "replace").splitlines(), 1
        ):
            if "=" not in line:
                continue
            value = line.split("=", 1)[1].strip()
            if value.startswith("StockVolumetricClouds/Clouds/") and (
                "/Textures/" in value or "/Sounds/" in value
            ):
                refs[value].append(f"{filename.rsplit('/', 1)[-1]}:{line_no}")
    return dict(sorted(refs.items()))


def audit(path: Path) -> dict:
    with zipfile.ZipFile(path) as z:
        entries = z.infolist()
        names = [i.filename for i in entries]
        files = {name: z.read(name) for name in names if name.endswith(".cfg")}
        clouds = next(
            (name for name in names
             if name.endswith("StockVolumetricClouds/Clouds/clouds.cfg")),
            None,
        )
        if clouds is None:
            raise ValueError("EVE clouds.cfg missing from release")
        cloud_text = files[clouds].decode("utf-8-sig", "replace")
        parsed = parse_nodes(cloud_text, clouds)
        layers = layers_from_nodes(parsed)
        refs = collect_paths(files)
        all_game_data = {
            name[name.index("GameData/") + len("GameData/"):]
            for name in names if "GameData/" in name
        }

    def exists(logical: str) -> bool:
        return any(name == logical or name.startswith(logical + ".")
                   for name in all_game_data)

    missing = [
        {"path": key, "used_by": sources}
        for key, sources in refs.items() if not exists(key)
    ]
    return {
        "reference_zip_filename": path.name,
        "sha256": fingerprint(path),
        "entry_count": len(entries),
        "cloud_layer_count": len(layers),
        "cloud_type_count": sum(x["type_count"] for x in layers),
        "painter_tile_count": sum(x["painter_tile_count"] for x in layers),
        "hermite_key_count": sum(x["curve_key_count"] for x in layers),
        "layers_per_body": dict(sorted(Counter(
            x["planet"] for x in layers
        ).items())),
        "referenced_cloud_asset_count": len(refs),
        "missing_cloud_asset_count": len(missing),
        "missing_cloud_texture_count": sum(
            "/Textures/" in x["path"] for x in missing
        ),
        "missing_cloud_sound_count": sum(
            "/Sounds/" in x["path"] for x in missing
        ),
        "layers": layers,
        "missing_assets": missing,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("zip", type=Path, help="Local user-owned EVE release")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--strict-manifest", action="store_true",
                    help="Check counts for the archived 23 August 2026 release")
    args = ap.parse_args()
    if not args.zip.is_file():
        ap.error(f"No such file: {args.zip}")
    try:
        report = audit(args.zip)
    except (OSError, ValueError, zipfile.BadZipFile) as exc:
        print(f"EVE reference audit failed: {exc}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(report, indent=2, ensure_ascii=False))
    else:
        for key in (
            "reference_zip_filename", "sha256", "entry_count",
            "cloud_layer_count", "cloud_type_count", "painter_tile_count",
            "hermite_key_count", "layers_per_body",
            "referenced_cloud_asset_count", "missing_cloud_asset_count",
            "missing_cloud_texture_count", "missing_cloud_sound_count",
        ):
            print(f"{key}: {report[key]}")
        print("Missing cloud assets (first 10):")
        for item in report["missing_assets"][:10]:
            print(f"  {item['path']} ({', '.join(item['used_by'][:2])})")
    if args.strict_manifest:
        expected = {
            "sha256": "d4c74123c76651969bc81073e3e6eb2e8236ae464a336618fd9baf8baebc42a7",
            "entry_count": 87,
            "cloud_layer_count": 19,
            "cloud_type_count": 70,
            "painter_tile_count": 27,
            "hermite_key_count": 293,
            "referenced_cloud_asset_count": 103,
            "missing_cloud_asset_count": 103,
            "missing_cloud_texture_count": 76,
            "missing_cloud_sound_count": 27,
        }
        differences = {
            k: (report.get(k), v) for k, v in expected.items()
            if report.get(k) != v
        }
        if differences:
            print(f"Reference manifest mismatch: {differences}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
