#!/usr/bin/env python3
"""Download curated CC0 scatter models from Poly Haven.

The downloader is intentionally conservative: it preflights the exact requested
glTF package and dependencies and rejects anything over the manifest's source-size
budget before writing a byte. It never silently falls upward to a higher texture
resolution, which previously made some VFX-oriented assets unexpectedly huge.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import time
from typing import Any, Iterable
from urllib.error import HTTPError, URLError
from urllib.parse import urlparse
from urllib.request import Request, urlopen

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = REPO_ROOT / "assets" / "scatter" / "asset_manifest.json"
API_ROOT = "https://api.polyhaven.com"
USER_AGENT = "AsterraScatterAuthoring/0.2 (Powered by Poly Haven)"
REQUEST_DELAY_S = 0.12
CHUNK_BYTES = 1024 * 1024
MIB = 1024 * 1024
RESOLUTION_ORDER = ("1k", "2k", "4k", "8k", "16k")


def read_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        value = json.load(handle)
    if not isinstance(value, dict):
        raise ValueError(f"Expected a JSON object in {path}")
    return value


def request_json(url: str) -> dict[str, Any]:
    request = Request(url, headers={"User-Agent": USER_AGENT, "Accept": "application/json"})
    try:
        with urlopen(request, timeout=60) as response:
            payload = json.loads(response.read().decode("utf-8"))
    except (HTTPError, URLError) as exc:
        raise RuntimeError(f"API request failed: {url}: {exc}") from exc
    if not isinstance(payload, dict):
        raise RuntimeError(f"Unexpected API response for {url}")
    return payload


def is_file_node(value: Any) -> bool:
    return isinstance(value, dict) and isinstance(value.get("url"), str)


def allowed_resolutions(requested: str) -> list[str]:
    """Requested resolution first, then lower resolutions only. Never go higher."""
    if requested not in RESOLUTION_ORDER:
        return [requested]
    index = RESOLUTION_ORDER.index(requested)
    return [requested, *reversed(RESOLUTION_ORDER[:index])]


def find_gltf_node(files: dict[str, Any], requested_resolution: str) -> tuple[str, dict[str, Any]]:
    gltf = files.get("gltf")
    if not isinstance(gltf, dict):
        raise RuntimeError("Poly Haven asset does not expose a glTF branch")

    for resolution in allowed_resolutions(requested_resolution):
        branch = gltf.get(resolution)
        if not isinstance(branch, dict):
            continue
        for format_key in ("gltf", "glb"):
            node = branch.get(format_key)
            if is_file_node(node):
                return resolution, node
        for node in branch.values():
            if is_file_node(node):
                url = str(node["url"]).lower()
                if url.endswith((".gltf", ".glb")):
                    return resolution, node

    raise RuntimeError(
        f"No glTF/GLB at requested resolution {requested_resolution} or lower; "
        "refusing to fall upward to a larger package"
    )


def safe_relative_path(raw: str) -> Path:
    clean = raw.replace("\\", "/").lstrip("/")
    path = Path(clean)
    if any(part == ".." for part in path.parts):
        raise RuntimeError(f"Unsafe dependency path returned by API: {raw}")
    return path


def filename_from_url(url: str, fallback: str) -> str:
    name = Path(urlparse(url).path).name
    return name or fallback


def md5_file(path: Path) -> str:
    digest = hashlib.md5()  # noqa: S324 - provider checksum, not cryptographic trust.
    with path.open("rb") as handle:
        while chunk := handle.read(CHUNK_BYTES):
            digest.update(chunk)
    return digest.hexdigest()


def iter_dependencies(node: dict[str, Any]) -> Iterable[tuple[Path, dict[str, Any]]]:
    includes = node.get("include", {})
    if not isinstance(includes, dict):
        return
    for raw_path, child in includes.items():
        if isinstance(raw_path, str) and is_file_node(child):
            yield safe_relative_path(raw_path), child


def package_nodes(root_node: dict[str, Any]) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    seen_urls: set[str] = set()
    for node in [root_node, *(child for _path, child in iter_dependencies(root_node))]:
        url = str(node.get("url", ""))
        if not url or url in seen_urls:
            continue
        seen_urls.add(url)
        result.append(node)
    return result


def package_size_bytes(root_node: dict[str, Any]) -> int:
    total = 0
    for node in package_nodes(root_node):
        size = int(node.get("size", 0) or 0)
        if size <= 0:
            raise RuntimeError(
                f"Provider did not publish a file size for {node.get('url', '?')}; "
                "refusing an unbounded download"
            )
        total += size
    return total


def download_file(node: dict[str, Any], destination: Path, dry_run: bool) -> None:
    url = str(node["url"])
    expected_md5 = str(node.get("md5", "")).lower()
    expected_size = int(node.get("size", 0) or 0)

    if destination.exists():
        size_ok = expected_size <= 0 or destination.stat().st_size == expected_size
        hash_ok = not expected_md5 or md5_file(destination) == expected_md5
        if size_ok and hash_ok:
            print(f"  cached  {destination.relative_to(REPO_ROOT)}")
            return

    print(f"  fetch   {destination.relative_to(REPO_ROOT)}")
    if dry_run:
        return

    destination.parent.mkdir(parents=True, exist_ok=True)
    temp = destination.with_suffix(destination.suffix + ".part")
    request = Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urlopen(request, timeout=180) as response, temp.open("wb") as handle:
            while True:
                chunk = response.read(CHUNK_BYTES)
                if not chunk:
                    break
                handle.write(chunk)
    except (HTTPError, URLError) as exc:
        temp.unlink(missing_ok=True)
        raise RuntimeError(f"Download failed: {url}: {exc}") from exc

    if expected_size > 0 and temp.stat().st_size != expected_size:
        temp.unlink(missing_ok=True)
        raise RuntimeError(f"Size mismatch for {destination.name}")
    if expected_md5 and md5_file(temp) != expected_md5:
        temp.unlink(missing_ok=True)
        raise RuntimeError(f"MD5 mismatch for {destination.name}")
    os.replace(temp, destination)


def selected_assets(
    manifest: dict[str, Any],
    ids: set[str],
    biome: str | None,
    priority: str | None,
    select_all: bool,
) -> list[dict[str, Any]]:
    raw_assets = manifest.get("assets", [])
    if not isinstance(raw_assets, list):
        raise ValueError("Manifest 'assets' must be an array")

    result: list[dict[str, Any]] = []
    for raw in raw_assets:
        if not isinstance(raw, dict) or not isinstance(raw.get("id"), str):
            continue
        asset_id = str(raw["id"])
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


def source_limit_mib(manifest: dict[str, Any], override: float | None) -> float:
    if override is not None:
        return override
    policy = manifest.get("selection_policy", {})
    if isinstance(policy, dict):
        return float(policy.get("max_source_package_mib", 100.0))
    return 100.0


def fetch_asset(
    asset: dict[str, Any],
    manifest: dict[str, Any],
    resolution_override: str | None,
    dry_run: bool,
    max_source_mib: float,
) -> None:
    asset_id = str(asset["id"])
    requested_resolution = resolution_override or str(asset.get("resolution", "1k"))
    source_root = REPO_ROOT / str(manifest.get("source_cache", "assets/scatter/source")) / asset_id

    print(f"\n{asset_id} [{asset.get('kind', 'unknown')}] requested={requested_resolution}")
    files_url = f"{API_ROOT}/files/{asset_id}"
    files = request_json(files_url)
    resolution, root_node = find_gltf_node(files, requested_resolution)
    total_bytes = package_size_bytes(root_node)
    total_mib = total_bytes / MIB
    file_count = len(package_nodes(root_node))
    print(f"  preflight {resolution}: {total_mib:.1f} MiB across {file_count} file(s)")
    if total_mib > max_source_mib:
        raise RuntimeError(
            f"source package is {total_mib:.1f} MiB, above Asterra's {max_source_mib:.1f} MiB cap"
        )

    root_url = str(root_node["url"])
    root_name = filename_from_url(root_url, f"{asset_id}.gltf")
    download_file(root_node, source_root / root_name, dry_run)
    for relative_path, child in iter_dependencies(root_node):
        download_file(child, source_root / relative_path, dry_run)

    if dry_run:
        return

    provenance = {
        "asset_id": asset_id,
        "provider": manifest.get("provider", "Poly Haven"),
        "license": manifest.get("provider_license", "CC0-1.0"),
        "asset_page": asset.get("url", f"https://polyhaven.com/a/{asset_id}"),
        "api_files_endpoint": files_url,
        "requested_resolution": requested_resolution,
        "resolved_resolution": resolution,
        "package_bytes": total_bytes,
        "package_mib": round(total_mib, 3),
        "package_file_count": file_count,
        "root_file": root_name,
        "retrieved_unix": int(time.time()),
        "user_agent": USER_AGENT,
    }
    source_root.mkdir(parents=True, exist_ok=True)
    with (source_root / "source.json").open("w", encoding="utf-8") as handle:
        json.dump(provenance, handle, indent=2, sort_keys=True)
        handle.write("\n")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Fetch curated Asterra scatter sources from Poly Haven")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--asset", action="append", default=[], help="Asset slug; may be repeated")
    parser.add_argument("--biome", help="Select assets assigned to one exact Asterra biome name")
    parser.add_argument("--priority", choices=("core", "secondary"), help="Filter manifest priority")
    parser.add_argument("--resolution", choices=RESOLUTION_ORDER)
    parser.add_argument("--max-source-mib", type=float, help="Override manifest package-size cap")
    parser.add_argument("--all", action="store_true", help="Select every asset passing other filters")
    parser.add_argument("--dry-run", action="store_true", help="Run API preflight but download nothing")
    parser.add_argument("--list", action="store_true", help="List matching assets without API calls")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    manifest = read_json(args.manifest.resolve())
    ids = set(args.asset)
    assets = selected_assets(manifest, ids, args.biome, args.priority, args.all)
    max_source_mib = source_limit_mib(manifest, args.max_source_mib)

    if args.list:
        raw_assets = manifest.get("assets", []) if not (ids or args.biome or args.priority or args.all) else assets
        for raw in raw_assets:
            if isinstance(raw, dict):
                print(f"{raw.get('id','?'):28} {raw.get('kind','?'):22} {raw.get('priority','?')}")
        print(f"\nsource package cap: {max_source_mib:.1f} MiB")
        return 0

    if not assets:
        print("No assets selected. Use --asset, --biome, or --all (or --list to inspect).", file=sys.stderr)
        return 2

    failures: list[tuple[str, str]] = []
    for index, asset in enumerate(assets):
        try:
            fetch_asset(asset, manifest, args.resolution, args.dry_run, max_source_mib)
        except Exception as exc:  # Continue batch preflight/download and report all failures.
            failures.append((str(asset.get("id", "?")), str(exc)))
            print(f"  REJECTED: {exc}", file=sys.stderr)
        if index + 1 < len(assets):
            time.sleep(REQUEST_DELAY_S)

    if failures:
        print("\nRejected/failed assets:", file=sys.stderr)
        for asset_id, message in failures:
            print(f"  {asset_id}: {message}", file=sys.stderr)
        return 1

    action = "Preflight complete" if args.dry_run else "Download complete"
    print(f"\n{action}: {len(assets)} asset(s), cap={max_source_mib:.1f} MiB each.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
