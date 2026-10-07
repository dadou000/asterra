+++
path = "/rendering/terrain/clipmaps/shaders"
title = "Clipmap vertex and pixel shaders"
kind = "concept"
status = "stable"
owner_module = "OrbitTerrainRender"
summary = """
The clipmap terrain pass is one vertex shader (ClipmapVertexShader.hpp) that builds each vertex from vertex ID plus a \
per-level sample buffer and 56 push-constant dwords, and one pixel shader (TerrainSurfaceShader.hpp, patched into bed and \
effect variants) that writes four G-buffer targets. Morphing, the finer level's hole and the cross-fade coverage all live \
in the vertex shader and travel to the pixel stage as clip distance and lodFade. Both are embedded HLSL strings compiled \
by DXC at renderer construction; saving either file takes the Studio generation handoff, not a shader swap."""
keywords = ["clipmap shader", "vertex shader", "pixel shader", "push constants", "DrawConstants", "morph", "hole", "lodFade", "horizonClip", "SV_ClipDistance", "dither", "g_samples", "sample buffer", "DXC", "embedded shader", "hot reload", "G-buffer", "SurfaceOutputs"]
sources = [
  "engine/terrain_render/src/ClipmapVertexShader.hpp",
  "engine/terrain_render/src/TerrainSurfaceShader.hpp",
  "engine/terrain_render/src/TerrainPreviewRenderer.cpp",
  "engine/terrain_stream/include/orbit/terrain_stream/TerrainSampleStreamer.hpp",
  "engine/shader/dxc/src/DxcShaderCompiler.cpp",
  "engine/hot_reload/src/ChangeClassifier.cpp",
  "engine/hot_reload/src/HotIterationService.cpp",
  "apps/editor/src/HotReloadBootstrap.cpp",
]
symbols = ["kClipmapVertexShader", "kTerrainSurfacePixelShader", "BuildDrawConstants", "PhysicalSampleIndex", "SurfaceDirectionForOffsetFromBasis", "lodFade", "horizonClip", "EncodeSurfaceMeta", "CreateTerrainPipeline", "ClassifyChange", "AddNativeHotRoot"]
invariants = [
  "The draw is index-less: 6 vertices per cell, (resolution-1)^2 cells per level (patchVertexCounts_), corner order (0,0) (0,1) (1,0) | (1,0) (0,1) (1,1). The water vertex shader's wet-corner test relies on this same order.",
  "Push constants are 56 dwords laid out by BuildDrawConstants and read as DrawConstants in the shader; the terrain and water pipelines both declare pushConstantDwords = 56. Adding a field means changing the HLSL struct, the store(...) indices and both pipeline declarations together.",
  "The per-level sample buffer (SRV slot 0, g_samples) holds 32-byte TerrainSampleValue records: elevation @0, morph target xy @4, biome weights @12 and @16, standing-water depth @20, fine slope east/north @24. The shader hard-codes these offsets; a change to TerrainSampleValue must change the shader.",
  "Samples are read toroidally: physical index = (logical + origin) % resolution, with origin from residency (push-constant .w of centerUp/centerEast). Never index g_samples with the raw logical coordinate.",
  "A vertex moves only horizontally while morphing: offsetMeters lerps to the sample's morph target by smoothstep of the Chebyshev distance between morphStart and morphEnd, and only when a coarser level exists (g_morph.z). Elevation is never morphed.",
  "Direction from metre offset must use SurfaceDirectionForOffsetFromBasis (series below 0.2 rad); see /rendering/terrain/clipmaps/precision-and-pages.",
  "Culling is done with clip distance, not discard: horizonClip < 0 removes the vertex. A cell inside the finer level's hole is culled only once finerFade >= 0.999; before that it is kept and lodFade.y = finerFade dithers it against the finer level.",
  "The pixel stage keeps a fragment when lodFade.y <= noise < lodFade.x (interleaved gradient noise of SV_Position.xy). Adjacent levels use complementary x/y, so a pixel belongs to exactly one level. lodFade is not in the base pixel shader; BuildClipmapBedPixelShader injects it (TEXCOORD13).",
  "The clipmap vertex shader writes spacingMeters = 0 and worldPosition = 0, which turns off the pixel shader's fake detail bump (ApplyDetailNormal fades to nothing at spacing 0); the real normal comes from the sample's fine slope.",
  "The VSOutput struct is written out in ClipmapVertexShader.hpp, and TerrainSurfaceShader.hpp; the water pixel shader instead copies it from the water vertex shader. TEXCOORD locations must stay in step; adding an interpolant means editing every hand-written copy.",
  "Both shaders are embedded raw-string constants compiled with shader::Compiler (debug = false) inside the TerrainPreviewRenderer constructor (CreatePipeline, CreateWaterPipeline); a DXC failure throws std::runtime_error from that constructor.",
]
related = [
  "/rendering/terrain/clipmaps/debugging",
  "/rendering/terrain/clipmaps/level-cross-fade",
  "/rendering/terrain/clipmaps/level-planning",
  "/rendering/terrain/clipmaps/distance-bands",
  "/rendering/terrain/clipmaps/precision-and-pages",
  "/rendering/terrain/surface-material",
  "/rendering/terrain/water-volume-shader",
  "/foundation/hot-reload",
]
depends_on = ["/rendering/terrain/clipmaps", "/rendering/terrain/streaming", "/rendering/shader-compiler", "/rules/hot-iteration"]
verify = [
  "ctest -R Orbit.TerrainSurfaceShader (compiles base, effect, bed, water vertex and water pixel variants with DXC and checks the bed variant draws no water).",
  "Switch on clipmap_hole_view, clipmap_projection_view and clipmap_shading_view one at a time (view.terrain_overlays_set) after any change to the debug branches of the vertex shader.",
]
verified = "55d48117"

[routes]
"the finer level's hole, fade or dither looks wrong" = "/rendering/terrain/clipmaps/level-cross-fade"
"vertex data looks corrupt (bad elevation, morph target or slope)" = "/rendering/terrain/clipmaps/debugging"
"effect stamps, soot, ash, wetness or heat on terrain" = "/rendering/terrain/surface-material"
"near-field water plane, shoreline or the uniform planet mesh" = "/rendering/terrain/water-volume-shader"

[[diagnose]]
symptom = "I edited ClipmapVertexShader.hpp or TerrainSurfaceShader.hpp and nothing changed in the running Studio"
steps = [
  "These are .hpp files under engine/, so ClassifyChange returns native-module; terrain_render is not one of the in-process native hot roots (HotReloadBootstrap.cpp registers only the probe and eye-adaptation roots), so the save is queued as the seamless Studio generation refresh (incremental build, then handoff).",
  "Look for the log line 'Hot iteration queued seamless Studio refresh for native-module: <path>' and then for the staged replacement; there is no shader-only pipeline swap for engine-owned shaders.",
  "If the build or the new generation fails, the old generation keeps running with the old shader: read the build output rather than assuming the edit was ignored.",
]
docs = ["/rules/hot-iteration", "/foundation/hot-reload"]

[[diagnose]]
symptom = "terrain disappears or a level draws nothing after a push-constant or sample layout change"
steps = [
  "Compare the HLSL DrawConstants struct with BuildDrawConstants: 16 dwords of g_mvp, then ten float4s (planet, centerUp+originX, centerEast+originY, centerNorth+morphStart, morph, debug, centerOffset+drySurface+seaLevel, observerEast+selfFade, observerUp+finerFade, observerNorth+zoneFraction) = 56.",
  "Compare the byte offsets used with g_samples.Load (0, 4, 12, 16, 20, 24 inside 32 bytes) with TerrainSampleValue.",
  "Run orbit_view_terrain_overlays_set(view_id, clipmap_sample_health=true) and clipmap_projection_view=true to see whether the data or the projection is bad.",
]
docs = ["/rendering/terrain/clipmaps/debugging"]

[[diagnose]]
symptom = "black or double-drawn gap between two levels"
steps = [
  "Switch on clipmap_hole_view: green = culled inside a finer level's hole, blue = inside the hole while the finer level fades in.",
  "Check the hole inputs: innerHoleHalfExtentMeters (g_morph.w) and the centre offset difference (g_morph.y, g_debug.w) are only set when a finer active level exists (finerMotion != nullptr).",
  "Check the fade pair passed by Draw: selfFade = LevelFade(level), finerFade = LevelFade(level - 1); in banded mode the same slots carry band edges in metres.",
]
docs = ["/rendering/terrain/clipmaps/level-cross-fade", "/rendering/terrain/clipmaps/debugging"]
+++

## Pipeline and bindings

`TerrainPreviewRenderer::Impl::CreatePipeline` compiles `kClipmapVertexShader` as the vertex shader and the pixel
shader `BuildSurfaceEffectPixelShader(BuildClipmapBedPixelShader(kTerrainSurfacePixelShader))` (see
`/rendering/terrain/surface-material` and `/rendering/terrain/water-volume-shader` for what those two patch in).
`CreateTerrainPipeline(wireframe)` then builds the graphics pipeline: triangle list, no vertex attributes, cull none, depth
test `GreaterEqual` with depth write (reverse-Z projection), four `RGBA16_Float` colour attachments, 56 push-constant
dwords and two shader-resource buffers. The wireframe variant is built lazily from the same bytecode.

| Slot | What | Set by |
| --- | --- | --- |
| buffer 0 | level sample buffer `g_samples` (`ByteAddressBuffer`, binding 0) | `SetGraphicsBuffer(0, levels_[i].gpuSampleBuffer)` per level |
| buffer 1 | surface-effect stamps (binding 1 in the pixel shader) | `surfaceEffects_.Bind` once per Draw |
| push constants | `DrawConstants`, 56 dwords | `BuildDrawConstants`, per level |

The loop in `Draw` issues one `Draw(patchVertexCounts_[level])` per active level; the water pass (`DrawWater`) reuses the
same constants with different fade values.

## Push constants (dword index: field)

0-15 `g_mvp` (row-major); 16-19 `g_planet` = planet radius, observer radius, level sample spacing, grid resolution;
20-23 centre up (observer-local) + residency origin X; 24-27 centre east + origin Y; 28-31 centre north + `morphStart`;
32-35 `g_morph` = morphEnd, finer-hole centre X, hasCoarser, finer-hole half extent; 36-39 `g_debug` = level index,
debug mode, side cut, finer-hole centre Y; 40-43 centre offset metres xy, drySurface, sea level;
44-47 observer east (body) + selfFade; 48-51 observer up + finerFade; 52-55 observer north + zone fraction.

`g_debug.y` selects the mode: 0 off, 1 level tint, 2 sample health, 3 hole view, 4 projection view, 5 shading view
(highest enabled wins). `zone fraction < 0` means the ladder (time fades); `>= 0` means experimental distance bands, where
selfFade/finerFade become the band's inner/outer edge in metres (`BandInner`, `BandOuter`, `BandZone`).

## Vertex stage

1. `vertexId / 6` gives the cell, `vertexId % 6` the corner; logical sample = cell + corner offset.
2. The sample is read through `PhysicalSampleIndex` (toroidal) from `g_samples`.
3. Local metre offset = `(logical - halfCells) * spacing`; world offset = `g_centerOffsetMeters.xy + local`.
4. If a coarser level exists, offset is lerped to the sample's morph target by `smoothstep` over
   `[morphStart, morphEnd]` of the Chebyshev distance (`max(|x|,|y|)`).
5. `SurfaceDirectionForOffsetFromBasis` turns the offset into a unit direction on the sphere; the camera-relative position
   is built in observer-local axes (x east, y up, z north) with `planetRadius * (cos - 1)` so the large radius cancels
   before it reaches float precision, then multiplied by `g_mvp`.
6. The normal is `normalize(direction - tangentEast * slopeEast - tangentNorth * slopeNorth)` using the sample's fine slope;
   the body-fixed normal and direction are rotated with the observer's east/up/north in body space.
7. `horizonClip = direction.y - saturate(R/observerR) + 2e-6 + max(elevation,0)/R`; negative means beyond the horizon.
8. The hole: the cell centre box (cell size included) lying fully inside the finer level's half extent sets
   `insideHole`. Ladder mode then either culls (`finerFade >= 0.999`) or sets `lodFade.y = finerFade`; banded mode computes
   `lodFade` from `smoothstep` of the vertex distance around the edges (zone fraction wide, plus a two-cell slack for culling).

The three debug modes overwrite `biome0/biome1` with a one-hot colour index, force `horizonClip = 1` and reset
`lodFade = (1, 0)`; the shading view instead sets `waterDepth = 5` as a flag. Their colour meanings are in
`/rendering/terrain/clipmaps/debugging`.

## Pixel stage and outputs

`kTerrainSurfacePixelShader` normalises the eight biome weights (order: ocean, desert, grassland, temperate forest, boreal
forest, tundra, alpine, wetland), blends a hard-coded palette (a second dry-world palette when `drySurface` is set) and
writes four targets: `SV_Target0` preview colour, `SV_Target1` base colour + roughness, `SV_Target2` body-fixed normal +
metallic, `SV_Target3` emission + `EncodeSurfaceMeta(surfaceClass, 1.0)` (class + representation/16; terrain = 1, water = 2).
Because the clipmap vertex shader writes `worldPosition = 0`, the dry-world dust noise (which reads `worldPosition`) is
evaluated at a constant point in the clipmap pass.

Known gap (read from the strings): `BuildClipmapBedPixelShader` replaces everything from the standing-water comment to the
first `SurfaceOutputs output;`, and in the base shader that range also contains the `input.waterDepth > 4.0` shading-view
branch, so the compiled clipmap pixel shader has no code that reports the flag the vertex shader sets for
`clipmap_shading_view`.

## Embedding, compile and hot iteration

- Shaders are C++ raw-string constants (`inline constexpr const char*`), split into adjacent literals; there is no
  `.hlsl` file. `TerrainPreviewRenderer.cpp` aliases them as `kVertexShader` / `kPixelShader`.
- `shader::Compiler::Compile` with entry point `main` and `debug = false`; the DXC backend targets SPIR-V for Vulkan 1.3
  (`-spirv`, `-fspv-target-env=vulkan1.3`, `-HV 2021`, `-O3` when not debug) and memoises per process by stage, shader model,
  flags, entry point and source text (`/rendering/shader-compiler`).
- Saving either header is a native-module change (`ClassifyChange`: `.hpp` -> `NativeModule`). `IsNativeHotPath` is false
  for it, so `HotIterationService` queues the seamless Studio refresh: incremental build, staged replacement generation,
  handoff. The running generation stays alive if the build fails. Only shader-extension files (`.hlsl`, `.glsl` and similar) under a project `content/` path
  take the direct shader route; engine-owned ones are `restart-required`, i.e. the same generation refresh.
- Derived variants are made by exact-text replacement; if a marker no longer matches, construction throws
  ("could not locate ... injection marker" or "Orbit water pass could not locate ...") instead of silently drawing wrong.
