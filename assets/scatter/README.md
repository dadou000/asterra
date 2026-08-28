# Scatter assets

This directory is the authoring/runtime boundary for Asterra's ecological scatter.

## Layout

```text
assets/scatter/
  asset_manifest.json       # automated Poly Haven CC0 acquisition + biome assignments
  external_candidates.json # individually verified CC0 candidates needing manual acquisition
  source/                   # downloaded source scans; authoring cache (gitignored)
  runtime/                  # optimized game-ready meshes/materials/metadata
```

`source/` is intentionally not the final game representation. Many source scans
contain hundreds of thousands or millions of triangles. They must be normalized,
LOD-generated and material-packed before being placed under `runtime/`.

## 1. Validate the catalog

Offline structure validation:

```bash
python tools/validate_scatter_manifest.py
```

Full Poly Haven slug/type/glTF verification:

```bash
python tools/validate_scatter_manifest.py --online
```

## 2. Fetch Poly Haven source assets

From the repository root:

```bash
python tools/fetch_scatter_assets.py --list
python tools/fetch_scatter_assets.py --all --priority core
```

Useful narrower passes:

```bash
python tools/fetch_scatter_assets.py --biome TAIGA --priority core
python tools/fetch_scatter_assets.py --asset boulder_01
python tools/fetch_scatter_assets.py --all --dry-run
```

The fetcher uses the Poly Haven public API with an Asterra-specific User-Agent,
downloads the selected glTF and its declared dependencies, verifies MD5 checksums
when provided, and records provenance in `source/<asset>/source.json`.

Entries in `external_candidates.json` are not fetched automatically. Each source
page was selected because it explicitly reports a CC0 license, but the asset must
be downloaded manually until a provider-specific acquisition tool is implemented.

## 3. Optimize through Blender

Once source assets are present:

```bash
python tools/optimize_scatter_assets.py --all --priority core --keep-going
```

If Blender is not on PATH:

```bash
python tools/optimize_scatter_assets.py --all --priority core --keep-going \
  --blender "C:/Program Files/Blender Foundation/Blender 4.5/blender.exe"
```

The batch wrapper invokes `tools/scatter_optimize_blender.py` in background mode.
The Blender stage:

- imports the source glTF/GLB;
- moves the pivot to XY bounds centre / ground Z;
- preserves provider LODs when they are identifiable;
- auto-decimates only safe opaque classes such as rocks and deadwood;
- deliberately refuses blind foliage/tree decimation when no provider LOD is found;
- creates simplified collision for interactable major opaque props;
- writes `runtime/<asset>/metadata.json` with bounds, triangle counts and LOD status.

Raw alpha-card foliage and tree crowns need foliage-aware LOD authoring or
impostors rather than generic triangle collapse.

## License

The assets selected in `asset_manifest.json` are Poly Haven assets released under
**CC0**. The manifest retains the source page for every asset. Keep generated
provenance files with the authoring cache even though attribution is not required
by CC0.

`external_candidates.json` records the license observed on each individual source
page. Do not assume every asset from an external provider has the same license.

The repository's own source code/license is independent of the CC0 status of these
third-party source assets. Do not replace an entry with an asset from another
provider unless its redistribution/game-use terms have been checked and recorded.

## Runtime rules

- Never point production scatter directly at `source/`.
- Keep real-world scale during authoring.
- Preserve foliage alpha coverage through mip levels.
- Prefer alpha hash/dithered cutout over transparent blending for vegetation.
- Generate simplified collision only for interactable major props.
- Trees require mesh LODs plus an impostor/far-canopy representation.
- The terrain/scatter classifier decides where an archetype may appear; source
  species names are not Asterra lore names.

See `docs/SCATTER_ECOLOGY_PLAN.md` for the full renderer and biome roadmap.
