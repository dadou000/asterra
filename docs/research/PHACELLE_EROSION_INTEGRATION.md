# Phacelle erosion in canonical Orbit terrain — experimental v153 port

Status: prototype on `feature/phacelle-erosion-v153`, based on `orbit` at
`b0a0de7fc3d93189dc512e404a65320bb8842866`.

## Scope and provenance

The v153 HTML playground uses a slope-guided Phacelle phase-noise filter
to produce branching gullies around tectonic mountains. The underlying
Phacelle filter is by Rune Skovbo Johansen (2025), **MPL 2.0**:
https://blog.runevision.com/2026/03/fast-and-gorgeous-erosion-filter.html

The adapted kernel lives in
`engine/terrain/src/PhacelleErosion.hpp` (MPL 2.0 derivative), with a
corresponding HLSL translation embedded in the canonical
`engine/terrain_gpu/src/FieldGenerationCompute.hpp`.
Keep attribution and MPL licensing when distributing these files.
The derivative is not a pixel-identical implementation of the HTML:
it uses Orbit's existing mountain/plate field and subtracts *no*
replacement-model height baseline, avoiding wholesale mountain sinking.

## Authoritative data flow

`AnalyticTerrainDesc::phacelle` ->
`AnalyticTerrainSource::Sample` / `GpuFieldGenerator` ->
canonical height, climate, material and physical terrain consumers ->
persistent GPU terrain cache / toroidal clipmaps.

The filter is strictly opt-in by recipe, not an alternative renderer,
independent height authority, or a shader-side decoration.

| Parameter | Range | Sandbox preset |
| --- | --- | --- |
| `strength` | 0–2, 0 disables | 0.85 |
| `scale` | (0, 8] | 1.0 |
| `octaves` | 0–5 | 4 |
| `lowlandFadeMeters` | >= 0, 0 disables fade | 2500 |

The native `AnalyticTerrainDesc` default is **0** strength. The integrated
Studio world enables the v153 preset through the schema properties
`Phacelle Erosion Strength/Scale/Octaves` and `Phacelle Lowland Fade`
(defaults 0.85 / 1.0 / 4 / 2500 m, `WorldSchemas.cpp`), read in
`SurfaceComposition.cpp`. Set strength to 0 on a terrain object to disable.
The sandbox preset in `apps/sandbox/src/Main.cpp` is unchanged.
The modifier is multiplied by the native tectonic mountain mask,
filtered continuously by footprint, smoothly faded in the lowlands
and clamped to +/-1000 m plus positive elevation headroom.

No simulation or procedural evaluation runs in the terrain vertex/pixel
shader. The existing streaming and LOD stitching are untouched.

## Validation

- CPU kernel compilation/determinism/finite values/footprint cutoff:
  `Orbit.PhacelleErosion`.
- GPU field parity: `Orbit.TerrainGpuField` uses nonzero Phacelle settings
  and compares GPU elevation with canonical CPU samples.
- Ensure normal scene LOD seams, world map, collisions, and offscreen
  visibility use the *same recipe revision*. Do not merge a second
  terrain generator.
- Profile GPU field-dispatch occupancy while streaming. Each sample
  currently participates in several slope probes; the Phacelle filter
  is intentionally opt-in until cost is measured and, if necessary,
  normal generation is separated from expensive elevation evaluation.

This branch has not yet passed a full native Windows/Vulkan build or
visual GPU test. Do not merge solely on static source inspection.

## Hot iteration

Changes to `engine/terrain/src/` and the embedded compute shader are
ordinary engine-owned C++ modifications and must use the existing
automatic native generation refresh / safe handoff path in
`docs/ORBIT_HOT_ITERATION.md`. Failed candidate builds must keep the
running generation alive. No separate launcher or parallel renderer.
