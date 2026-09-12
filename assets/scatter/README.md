# Scatter assets

This directory is the authoring/runtime boundary for Asterra terrain scatter.

## Current baseline

The previous scatter catalog was removed and replaced by a deliberately curated
CC0 temperate-forest library sourced through the Poly Haven API. The manifest is
the source of truth for runtime binding and authoring controls.

The production `TerrainScatter` autoload uses
`gpu_terrain_scatter_authoring_empty.gd` (the compatibility autoload name):

- file-backed foliage, grass, moss, trees and deadwood: loaded from the manifest;
- file-backed rocks and boulders: loaded from the manifest;
- built-in procedural grass fallback: **disabled**;
- built-in procedural stone fallbacks: **disabled**;
- terrain/scatter placement infrastructure: **active**.

`assets/scatter/runtime/` contains game-ready GLB LODs and textures produced by
the optimizer. Provider downloads remain in the gitignored `source/` cache.

## Layout

```text
assets/scatter/
  asset_manifest.json       # curated runtime catalog and placement settings
  external_candidates.json # replacement candidate list; intentionally empty
  source/                   # optional downloaded authoring cache (gitignored)
  runtime/                  # created when replacement runtime assets are built
```

## Authoring assets

Use the in-game **Terrain > Scatter Library** tab or the
`studio_scatter_library` MCP tool to:

1. Add a Poly Haven model slug and assign its biome and scatter kind.
2. Fetch the checksum-verified CC0 source package.
3. Build optimized game-ready LODs.
4. Tune density, spacing, scale and slope constraints.
5. Hot-reload the runtime and review it in the viewport.

`allow_partial_catalog=true` permits focused work on one biome without pretending
that every global biome is complete. Clear it before production-wide validation.

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

Runtime assets in this catalog come from Poly Haven under CC0 1.0. The manifest
records the provider and direct source page for each asset.
