+++
path = "/rendering/terrain/impacts/video-coverage"
title = "Moon-generation video: modifiers, implementation and bake cost"
kind = "reference"
status = "draft"
summary = "Transcript-to-code coverage for crater morphology, chronology, extended rays, maria and ice fractures, with explicit remaining simulation/rendering gaps and optimization tiers."
owner_module = "OrbitTerrainImpacts"
keywords = ["moon", "video", "modifiers", "optimization", "rays", "maria", "trajectories", "Europa", "Callisto"]
sources = [
  "engine/terrain_impacts/src/ImpactField.cpp",
  "engine/terrain_impacts/src/ImpactFieldToml.cpp",
  "engine/terrain_gpu/src/GeologyCompute.hpp",
  "engine/terrain_gpu/src/GpuGeologyCompiler.cpp",
  "engine/terrain_bake/src/TerrainBakeService.cpp",
  "engine/terrain_gpu/src/FieldGenerationCompute.hpp"]
related = ["/rendering/terrain/impacts", "/rendering/terrain/bake", "/rendering/terrain/gpu-passes"]
verified = "f4a2dc61"
+++

The supplied transcript is the feature checklist; it is not a scientific
specification. Orbit uses deterministic engineering approximations for crater
scaling and evolution. This inventory distinguishes compiled process data
from finished visible planetary materials.

Optimization tiers describe actual work, not promises of a frame rate:

| Tier | Work |
|---|---|
| P0 | Compile recipe parameters, seeds and stable IDs once. |
| P1 | Prepare frames, profile constants and spherical support per event. |
| P2 | Query a conservative tile cap, reuse worker scratch, evaluate local candidates only. |
| P3 | Compile chronological tile samples on GPU, retain CPU reference/fallback. |
| P4 | Persist checksummed geology tiles and sample the cached scale-space pyramid at runtime; rebake only changed support. |

| Video modifier | Existing implementation / change in this pass | Optimization |
|---|---|---|
| Broad spherical relief noise | Existing planetary noise stack in canonical directions. | P0; footprint filtering; cached terrain pages. |
| Smaller relief noise | Existing detail octaves; separate from authored impact events. | Stop below requested footprint; cached pages. |
| Excavation bowl and depth | Simple/complex profiles with radius-relative depth. | P1-P4; dimensionless analytic profile. |
| Raised rim | Smooth Gaussian ridge. | P1-P4; local finite event support. |
| Rim/floor/outer blending | Smooth bowl, rim and ejecta transitions exist; three independently tunable blend-width controls from the video do not. | Use compact profile parameters when added; no full-planet pass. |
| Younger crater overwrites older | Shared oldest-to-youngest impact/resurfacing composition. | P2-P4; sort candidate records, not the planet per texel. |
| Launch meteor and collide with existing terrain | **Missing.** Placement currently uses equal-area directions or authored centers; there is no terrain-aware projectile collision baker. | Future: broad-phase sphere miss rejection, conservative terrain hierarchy, first-hit traversal against a historical surface checkpoint; store resulting events. Radial height clearance must not be treated as a Euclidean distance field. |
| Projectile size/speed determines crater | Existing gravity/strength/density fit; angle convention corrected in this pass. | P0-P1; scale once per authored impact. |
| Giant impact basin | Complex excavation and multiple-ring relief exist. | P1-P4; resolve profile once, cap-query region. |
| Maria: connected lava burial | Existing chronological connected lava polylines erase older relief and cover. **Partial:** no basin-level lava fill simulation or maria-specific palette. | P2-P4; future basin fill should use local spill/connectivity data and cached deposits. |
| Noisy maria boundary and raised perimeter | Crater irregularity/rims exist; no independent irregular connected maria-region generator. | Future: generate region boundary once, index segments, bake material/height masks. |
| Maria age resets crater history | Existing flow age/order; freshly formed surfaces now keep zero micro-impact exposure. | P2-P4; one chronology, no separate post-bake crater deletion. |
| Age degradation / shallowing | Existing environmental age attenuation and explicit degradation; generated formation times now span the surface history. **Partial:** attenuation is not spatial diffusion or rim broadening. | P0-P4; future diffusion on dirty tiles with halos and bounded iteration count. |
| Irregular crater outlines | Existing deterministic periodic angular shape distortion. | P1-P4; canonical tangent frame, finite support. |
| Grazing elongated crater | Existing oriented elliptical profile; now consistent with size scaling. | P1-P4; prepared azimuth sine/cosine and elongation. |
| Binary impact pair | Existing deterministic companion event. **Partial:** both still compose sequentially by stable ID; simultaneous mutually suppressed rims need a grouped event evaluator. | Future: prepare a two-lobe grouped event once, query its union support, compose it atomically on CPU/GPU. |
| Central peaks | Existing complex-crater rebound profile. | P1-P4; profile resolution once. |
| Short ejecta rays | Existing ray/debris channel; same angular profile on CPU/GPU. | P1-P4; reuse tangent offset; exact undistorted angular mean instead of 256 trigonometric integration samples. |
| Very long, irregular rays | **Added:** independent `ray_extent_radii` and `ray_irregularity`, smooth radial fading, conservative tile bounds and regional invalidation. | P1-P4; material signal outside the massive blanket, no extended relief solve or per-frame events. |
| Chunk-normal seams | Existing cross-face gutters and canonical geometry derivatives; this pass does not change rendering normals. | Shared halo data; recompute dirty geometry only. |
| Callisto saturated / darker palette | **Missing as a geological presentation preset.** Process channels are available, but the surface renderer does not yet display ray/material-age coloration. | Future: compile albedo/roughness/cover channels into cached material tiles; compose them with geological materials, not random biome tints. |
| Europa ice coloration / equatorial bias | **Missing as a material preset.** Ice fracture relief is available. | Future: cached composition maps driven by latitude, exposure and fracture deposits. |
| Europa winding cracks | Existing seeded stress-guided spherical curves, grooves, flanking ridges and damage. | P1-P4; generated/indexed segments and local tile batches. |
| Europa smoothing / resurfacing | Existing chronological ice renewal and glacial processes; not a complete shell/ocean or tidal evolution simulation. | Cached renewed surfaces; dirty-region process passes. |
| Phobos / Hyperion body shape | Separate small-body concern; not implemented by spherical `ImpactField`. | Future: reuse semantic impacts on a body-shape surface adapter with its own acceleration structure. |
| Iapetus hemispheric palette | **Missing as a planetary material preset.** | Future: bake a body-frame hemisphere/exposure map once. |

Next implementation seams are terrain-aware trajectory placement, grouped
binary morphology, basin-level maria deposits, then consumption of geological
material channels in the surface shader. These are real remaining features;
compiling relief or a ray process field alone does not complete them.
