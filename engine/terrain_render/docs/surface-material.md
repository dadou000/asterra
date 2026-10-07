+++
path = "/rendering/terrain/surface-material"
title = "Surface material, physical surface and surface effects in terrain shading"
kind = "concept"
status = "stable"
owner_module = "OrbitTerrainRender"
summary = """
Of the surface files in terrain_render only the surface effects (M38 stamps) reach the GPU: SurfaceEffectGpuBinding \
uploads up to 512 body-fixed stamps to buffer slot 1 and BuildSurfaceEffectPixelShader splices a coating/emission stage \
into the terrain pixel shader just before G-buffer output. PhysicalSurface (M18) is a CPU adapter that packs an \
exposed-surface state for a renderer; no shader or draw path reads it yet, and terrain colour still comes from the \
eight packed biome weights. The M21 SurfaceMaterial adapter was removed in 0.0.9 (nothing linked it)."""
keywords = ["surface effects", "stamps", "wetness", "soot", "ash", "sediment", "heat", "M38", "M18", "physical surface", "SurfaceEffectGpuStamp", "g_surfaceEffects", "biome weights", "emission", "coating", "BuildSurfaceEffectPixelShader"]
sources = [
  "engine/terrain_render/src/PhysicalSurface.cpp",
  "engine/terrain_render/src/SurfaceEffects.cpp",
  "engine/terrain_render/src/SurfaceEffectShader.cpp",
  "engine/terrain_render/src/SurfaceEffectGpuBinding.cpp",
  "engine/terrain_render/include/orbit/terrain_render/SurfaceEffectGpuBinding.hpp",
  "engine/terrain_render/include/orbit/terrain_render/SurfaceEffects.hpp",
  "engine/studio_ui/src/StudioViewportRendererBase.cpp"]
symbols = [
  "MakePhysicalSurfaceRenderInput",
  "SurfaceEffectGpuStamp",
  "EvaluateSurfaceEffects",
  "ApplySurfaceEffects",
  "BuildSurfaceEffectPixelShader",
  "SurfaceEffectGpuBinding",
  "MaximumStampCount",
  "GraphicsBufferSlot",
  "BuildVolumeSurfaceEffectRenderBatch"]
invariants = [
  "PhysicalSurfaceRenderInput is built only from the canonical ExposedSurfaceState (MakePhysicalSurfaceRenderInput) so rendering never picks rock identity from biome weights; BedrockExposed() is simply material == Bedrock.",
  "PhysicalSurfaceRenderInput is not consumed by a shader or by TerrainPreviewRenderer at this revision (only tests include it). The visible terrain colour comes from biome0/biome1 and drySurface in TerrainSurfaceShader.hpp; changing a material/physical-surface value cannot change pixels until a binding is written.",
  "SurfaceEffectGpuStamp is 32 bytes (static_assert) and matches the HLSL SurfaceEffectStamp field for field: float3 body-fixed direction, angular radius (rad), amount, effect id, two reserved floats. Stamps store a unit body-fixed direction, not a metre position, so footprints stay sub-metre at planet scale and survive floating-origin shifts.",
  "SurfaceEffectKind values are the shader's integer ids: Wetness 0, Soot 1, Ash 2, Sediment 3, Heat 4. Adding a kind changes the enum, SurfaceEffectInfluence, EvaluateSurfaceEffects, ApplySurfaceEffects and the HLSL if/else chain together.",
  "Capacity is SurfaceEffectGpuBinding::MaximumStampCount = 512, equal to the shader's loop bound; Set copies at most 512 stamps and zero-fills the rest. angularRadiusRadians <= 0 is the end sentinel: the shader breaks at the first such entry, so valid stamps must be packed first with a positive radius.",
  "Influence of one stamp is amount * (1 - angle/radius)^2 inside the radius and 0 outside; wetness/soot/ash/sediment sum then saturate to [0,1], heat sums and is only floored at 0. The CPU functions and the HLSL implement the same formula and the same coating constants; edit both and Orbit.TerrainSurfaceEffects together.",
  "Effects touch only the exposed ground: the shader multiplies every influence by (1 - waterCoverage). In the clipmap bed variant waterCoverage is the constant 0 (water is a separate pass), and the water pass does not bind the stamp buffer at all.",
  "The effect stage is injected by exact-text replacement (struct VSOutput marker, the first `SurfaceOutputs output;` line, and the emission w-component line); each missing marker throws std::runtime_error ('could not locate terrain shader injection marker'). It must be applied after BuildClipmapBedPixelShader on the clipmap path.",
  "Bind writes the whole 512-stamp snapshot into a per-frame-in-flight host-visible structured buffer (frameIndex < framesInFlight, else std::out_of_range) and binds it at GraphicsBufferSlot 1, which is shader binding(1, 0); the terrain and uniform-planet pipelines both declare two shader-resource buffers for this."]
related = [
  "/rendering/terrain/clipmaps/shaders",
  "/rendering/terrain/water-volume-shader",
  "/rendering/volumes/render",
  "/world/surface-composition",
  "/rendering/terrain/scatter",
  "/legacy/v0-0-7-spec/m38-particle-surface-output-coupling"]
depends_on = ["/rendering/terrain/clipmaps", "/rendering/terrain/material-column", "/rendering/rhi"]
verify = [
  "ctest -R Orbit.TerrainSurfaceEffects (CPU falloff, saturation and coating results).",
  "ctest -R Orbit.TerrainSurfaceShader (the effect variant compiles with DXC).",
  "In Studio, toggle surface_effects (orbit_view_terrain_layers_set) with stamps present: the coating must appear and vanish with it."]
verified = "55d48117"

[routes]
"terrain has the wrong base colour or biome palette" = "/rendering/terrain/clipmaps/shaders"
"standing water colour, shoreline or silt under water" = "/rendering/terrain/water-volume-shader"
"where the stamps come from (particles, volume solver)" = "/rendering/volumes/render"

[[diagnose]]
symptom = "a surface effect (soot, ash, wetness, sediment, heat) does not show on the terrain"
steps = [
  "Check the view's surface_effects layer is on (view.terrain_layers_get / _set): the Studio passes an empty stamp list to SetSurfaceEffects when it is off.",
  "Check the stamps reach the renderer: the batch is built by BuildVolumeSurfaceEffectRenderBatch(body, stamps, MaximumStampCount) and SurfaceEffectGpuBinding::ActiveStampCount() is the number uploaded (capped at 512).",
  "Check each stamp has angularRadiusRadians > 0 and amount > 0, a normalised body-fixed direction and effect id 0..4, and that no zero-radius stamp precedes it (the shader stops at the first one).",
  "Remember water-covered pixels show no effect (influence * (1 - waterCoverage)) and the water pass never binds the stamps."]
docs = ["/rendering/volumes/render", "/rendering/terrain/clipmaps/shaders"]

[[diagnose]]
symptom = "a value in PhysicalSurfaceRenderInput has no effect on the rendered terrain"
steps = [
  "Confirm with a search for MakePhysicalSurfaceRenderInput that no renderer code consumes the result: at this revision only tests call it.",
  "Terrain colour is computed in TerrainSurfaceShader.hpp from the eight biome weights packed in each sample (TerrainSampleValue.biomeWeights0/1) and drySurface; to make material or physical-surface data visible it has to be packed into the sample buffer or bound as a new resource, with the shader and BuildDrawConstants/pipeline resource counts updated together."]
docs = ["/rendering/terrain/clipmaps/shaders", "/rendering/terrain/material-column"]

[[diagnose]]
symptom = "terrain construction throws 'could not locate terrain shader injection marker'"
steps = [
  "Someone edited the text of TerrainSurfaceShader.hpp that SurfaceEffectShader.cpp matches (struct VSOutput, SurfaceOutputs output;, or the emissionClass zero/EncodeSurfaceMeta lines).",
  "Restore the text or move the marker in SurfaceEffectShader.cpp, then run ctest -R Orbit.TerrainSurfaceShader."]
docs = ["/rendering/terrain/clipmaps/shaders"]
+++

## What each file does

| File | Role | Reaches the GPU? |
| --- | --- | --- |
| `PhysicalSurface.cpp` | `MakePhysicalSurfaceRenderInput`: copies material kind, substrate/exposed rock, exposed layer depth, moisture, standing-water depth and snow depth from `ExposedSurfaceState` | no |
| `SurfaceEffects.cpp` | CPU reference of the effect maths: `EvaluateSurfaceEffects` (stamp falloff) and `ApplySurfaceEffects` (coating on a `SurfacePbrState`) | no (tests and parity) |
| `SurfaceEffectShader.cpp` | `BuildSurfaceEffectPixelShader`: HLSL declarations plus the material stage injected into the pixel shader | yes |
| `SurfaceEffectGpuBinding.cpp` | owns the per-frame stamp buffers, `Set` (snapshot), `Bind` (upload and bind slot 1) | yes |

## How the effect stage binds into shading

1. Studio fills the renderer each frame: `terrain.renderer->SetSurfaceEffects(...)` with the batch built from the shared
   volume surface-effect stamps, or an empty list when the view's `surface_effects` layer is off
   (`StudioViewportRendererBase.cpp`). `UniformPlanetRenderer` has the same `SetSurfaceEffects`.
2. `Draw` calls `surfaceEffects_.Bind(commandList, frameIndex)` before the level loop; the buffer is bound at graphics buffer
   slot 1 while slot 0 is the level's sample buffer.
3. The pixel shader is built as `BuildSurfaceEffectPixelShader(base)` where `base` is the raw terrain pixel shader
   (uniform planet) or the bed variant (clipmap). The builder prepends the `SurfaceEffectStamp` struct,
   `g_surfaceEffects` (`StructuredBuffer`, binding 1) and an `EvaluateSurfaceEffects(float3)` loop, and replaces the first
   `SurfaceOutputs output;` with the material stage.
4. The stage evaluates the effects at `input.bodyFixedSurfaceDirection`, multiplies them by `1 - waterCoverage`, then in
   order: wetness darkens base colour and lit colour by 0.52 and pulls roughness to 0.12; soot lerps to a near-black with
   roughness 0.94 and removes metallic; ash lerps to light grey with roughness 0.98 and removes metallic; sediment lerps to
   brown with roughness 0.90; heat (capped at 8) adds emission to the emission target and 0.15 of it to the colour, and
   raises roughness a little.
5. The final replacement writes the emission rgb into `emissionClass.xyz`; the class/representation byte in `.w` is
   unchanged.

The G-buffer meaning of the targets is described in `/rendering/terrain/clipmaps/shaders`.

## Not yet wired

`PhysicalSurfaceRenderInput` is a renderer-facing contract that accepts only an `ExposedSurfaceState`, never biome weights
or a raw geology override. Today only tests construct it; shading is driven by biome weights.
