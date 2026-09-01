# Scatter assets

This directory is the authoring/runtime boundary for Asterra terrain scatter.

## Current baseline

The runtime scatter asset library is intentionally **empty** as of 2026-09-01.
The previous foliage, deadwood and geological assets have all been removed so the
library can be rebuilt deliberately from a clean baseline.

The production `TerrainScatter` autoload uses
`gpu_terrain_scatter_authoring_empty.gd`:

- file-backed foliage assets: **none**;
- file-backed geological assets: **none**;
- built-in procedural grass fallback: **disabled**;
- built-in procedural stone fallbacks: **disabled**;
- terrain/scatter placement infrastructure: **kept intact**.

`assets/scatter/runtime/` currently has no tracked files, so Git does not retain the
empty directory. Asset tooling will recreate it when new runtime assets are built.

## Layout

```text
assets/scatter/
  asset_manifest.json       # intentionally empty curated runtime catalog
  external_candidates.json # replacement candidate list; intentionally empty
  source/                   # optional downloaded authoring cache (gitignored)
  runtime/                  # created when replacement runtime assets are built
```

## Adding replacement assets

Reintroduce foliage or geology deliberately rather than restoring the old catalog:

1. Set `allow_empty_catalog` to `false` when the first replacement asset is ready.
2. Add the asset to `asset_manifest.json` with source, license, biome use,
   resolution and priority.
3. Validate with `python tools/validate_scatter_manifest.py`.
4. Fetch/import the source and optimize it into `runtime/<asset-id>/`.
5. Add it to the active runtime scatter binding only after LOD, material,
   collision and performance behavior have been reviewed.

While `allow_empty_catalog=true`, validation requires both `assets: []` and an
empty runtime asset tree. This prevents accidental partial reintroduction.

The existing acquisition/optimization tools remain available:

```bash
python tools/validate_scatter_manifest.py
python tools/fetch_scatter_assets.py --asset <asset-id>
python tools/optimize_scatter_assets.py --asset <asset-id>
```

## Asset budgets

- maximum source package: **100 MiB**;
- maximum individual runtime file: **100 MiB**;
- maximum runtime LOD0: **750,000 triangles**;
- preferred runtime LOD0: **250,000 triangles or less**.

These are upper authoring limits. Dense foliage and frequently repeated geology
should normally be substantially cheaper and use appropriate LODs/impostors.

## Runtime rules

- Never point production scatter directly at `source/`.
- Preserve real-world scale.
- Keep foliage alpha coverage stable through mip levels.
- Prefer alpha hash/dithered cutout over transparent blending for vegetation.
- Generate collision only where gameplay requires it.
- Trees need mesh LODs and a far/impostor representation before being accepted.
- The terrain/scatter classifier decides where an archetype may appear.

## License

There are currently no third-party runtime scatter assets in the catalog. Every
replacement asset must have its license and redistribution/game-use terms checked
and recorded before it is added.
