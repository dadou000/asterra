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

`source/` is intentionally not the final game representation. Source assets must
still be normalized/optimized before they can appear under `runtime/`.

## Hard asset budgets

The manifest now carries conservative authoring budgets:

- maximum exact downloaded source package: **100 MiB**;
- maximum individual runtime file: **100 MiB**;
- maximum runtime LOD0: **750,000 triangles**;
- preferred runtime LOD0: **250,000 triangles or less**.

The goal is to keep realistic game assets while rejecting VFX-oriented scan and
geometry-node sources that happen to be free but are inappropriate for large-scale
scatter. Examples removed from the curated list include multi-million-triangle
fir/pine/coastal trees and 1-2M triangle grass/wildflower sources.

## 1. Validate the catalog

Offline structure validation:

```bash
python tools/validate_scatter_manifest.py
```

Full Poly Haven slug/type/glTF verification:

```bash
python tools/validate_scatter_manifest.py --online
```

## 2. Preflight and fetch Poly Haven source assets

Before downloading anything, inspect the exact 1K glTF package sizes:

```bash
python tools/fetch_scatter_assets.py --all --priority core --dry-run
```

The dry run calls the Poly Haven files API and sums the root glTF/GLB plus declared
dependencies. Anything over the 100 MiB source-package cap is rejected before a
byte is downloaded. The fetcher also never silently falls upward from 1K to 2K/4K.

Then fetch the accepted assets:

```bash
python tools/fetch_scatter_assets.py --all --priority core
```

Useful narrower passes:

```bash
python tools/fetch_scatter_assets.py --biome TAIGA --priority core
python tools/fetch_scatter_assets.py --asset boulder_01
```

Downloaded files are verified with provider MD5/size metadata when available and
provenance is written to `source/<asset>/source.json`.

Entries in `external_candidates.json` are not fetched automatically. Each source
page was selected because it explicitly reports a CC0 license, but the asset must
be downloaded manually until a provider-specific acquisition tool is implemented.

## 3. Clean an older local cache

If an earlier manifest already downloaded assets that are no longer accepted:

```bash
python tools/prune_scatter_cache.py
python tools/prune_scatter_cache.py --apply
```

The first command is read-only. `--apply` removes source/runtime directories that
are no longer in the curated manifest, plus generated runtime assets that exceed
the current file-size or LOD0 triangle budgets.

## 4. Optimize through Blender

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
- deliberately avoids blind foliage/tree decimation when no provider LOD exists;
- creates simplified collision for interactable major opaque props;
- writes `runtime/<asset>/metadata.json` with bounds, triangle counts and LOD status.

After each build, the wrapper validates LOD0 triangles and every generated file
against the manifest budgets. Rejected output is deleted immediately so it cannot
be staged accidentally by a broad `git add`.

## License

The automated assets selected in `asset_manifest.json` are Poly Haven assets
released under **CC0**. The manifest retains the source page for every asset.

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
