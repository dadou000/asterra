# Scatter assets

This directory is the authoring/runtime boundary for Asterra terrain scatter.

## Current baseline

The foliage library is intentionally **empty** as of 2026-09-01. The previous grass,
shrubs, plants, trees, roots, stumps, deadwood and other organic scatter assets were
removed so replacement foliage can be introduced gradually and reviewed one asset
at a time.

The production `TerrainScatter` autoload currently uses
`gpu_terrain_scatter_authoring_geology_only.gd`:

- file-backed foliage assets: **disabled / none**;
- built-in procedural grass fallback: **disabled**;
- geological rocks/stones: **still enabled**;
- terrain/scatter placement infrastructure: **kept intact**.

## Layout

```text
assets/scatter/
  asset_manifest.json       # current curated runtime catalog; geology only for now
  external_candidates.json # replacement candidate list; intentionally empty
  source/                   # optional downloaded authoring cache (gitignored)
  runtime/                  # optimized game-ready assets; currently geology only
```

## Adding replacement foliage

Add vegetation deliberately rather than restoring the old catalog wholesale:

1. Add the candidate to `asset_manifest.json` with its source, license, biome use,
   resolution and priority.
2. Validate the catalog with `python tools/validate_scatter_manifest.py`.
3. Fetch or import the source and optimize it into `runtime/<asset-id>/`.
4. Add the asset to the active foliage runtime binding only after its LOD, material,
   wind, alpha and performance behavior have been reviewed.
5. Re-enable procedural/mesh foliage families explicitly when their replacement
   assets are ready.

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

These are upper authoring limits, not recommended foliage targets. High-density
vegetation should normally be substantially cheaper and use appropriate cards,
cluster meshes, LODs and impostors.

## Runtime rules

- Never point production scatter directly at `source/`.
- Preserve real-world scale.
- Keep foliage alpha coverage stable through mip levels.
- Prefer alpha hash/dithered cutout over transparent blending for vegetation.
- Generate collision only where gameplay requires it.
- Trees need mesh LODs and a far/impostor representation before being accepted.
- The terrain/scatter classifier decides where an archetype may appear.

## License

Current geological assets in `asset_manifest.json` retain their source/license
metadata. Every replacement foliage asset must be checked independently before it
is added to the runtime catalog.
