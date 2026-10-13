# Close-range terrain stability

## Existing owners and insertion points

- `SurfaceEffects` and `SurfaceEffectShader` own the CPU/GPU stamp falloff.
  Replace angular distance only; preserve the 32-byte stamp ABI and material model.
- `WaterVolumeShader::BuildClipmapBedPixelShader` owns removal of standing-water
  shading. Stop at the shading diagnostic, preserving its existing emission path.
- `TerrainSampleStreamer` owns CPU clipmap sampling. Morph normals toward the
  parent's footprint and spacing, matching the existing GPU generator.
- `TerrainPreviewRenderer::CompositeRegionDelta` owns dispatch ordering. Add
  UAV dependencies before and after actual delta composition, keeping the
  no-delta path free of extra barriers.

## Canonical state and exclusions

Body-fixed stamps, `TerrainSource` and clipmap requests remain authoritative.
No second terrain source, renderer, diagnostic control, water surface, watcher,
or geometry system. Broader post-composite normal reconstruction and adaptive
feature-driven geometry require separately validated changes.

## Validation and reflection

Add regressions for planetary stamp falloff, bed shader diagnostic preservation,
and CPU parent-normal agreement. Compile emitted variants with DXC and run docs
checks. Windows Studio/device rendering remains required for visual acceptance.
Existing native/embedded shader saves use the central automatic Studio-generation
handoff; no new ABI, state ownership, GPU resource lifetime or reload route.
