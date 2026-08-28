# Scatter assets

This directory is the authoring/runtime boundary for Asterra's ecological scatter.

## Layout

```text
assets/scatter/
  asset_manifest.json       # source-of-truth acquisition + biome assignments
  source/                   # downloaded source scans; authoring cache
  runtime/                  # optimized game-ready meshes/materials/metadata
```

`source/` is intentionally not the final game representation. Many source scans
contain hundreds of thousands or millions of triangles. They must be normalized,
LOD-generated and material-packed before being placed under `runtime/`.

## Fetch source assets

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

## License

The assets selected in `asset_manifest.json` are Poly Haven assets released under
**CC0**. The manifest retains the source page for every asset. Keep generated
provenance files with the authoring cache even though attribution is not required
by CC0.

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
