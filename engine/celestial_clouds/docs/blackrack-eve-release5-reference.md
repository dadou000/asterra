+++
path = "/rendering/clouds/references/blackrack-eve-release5"
title = "Blackrack EVE Release 5: inspected implementation evidence"
kind = "concept"
status = "research"
owner_module = "OrbitCelestialClouds"
summary = "Evidence-ranked technical examination of the user-supplied 2026-08-26 EVE Release 5 + Scatterer binary distribution, without copying proprietary assets or code."
keywords = ["blackrack", "EVE", "Scatterer", "release 5", "raymarched clouds", "temporal upscaling", "light volume", "overlap", "noise", "SDF", "cloud references"]
sources = ["engine/celestial_clouds/src/CloudRenderer.cpp", "engine/celestial_clouds/include/orbit/celestial_clouds/CloudRenderer.hpp", "engine/celestial_clouds/docs/raymarch.md", "engine/celestial_clouds/docs/shadows-and-light-volume.md"]
related = ["/rendering/clouds", "/rendering/clouds/references/blackrack-eve-orbit-design", "/rendering/clouds/references/library"]
depends_on = ["/rendering/clouds"]
[routes]
"what is actually inside blackrack release 5" = "/rendering/clouds/references/blackrack-eve-release5"
"how to apply blackrack techniques to Orbit" = "/rendering/clouds/references/blackrack-eve-orbit-design"
+++

# Scope, evidence, and legal boundary

This is an original engineering analysis of the archive supplied in the project conversation: RaymarchedVolumetricsEarlyAccess23_08_26.zip, SHA-256 d4c74123c76651969bc81073e3e6eb2e8236ae464a336618fd9baf8baebc42a7. It is a Release 5 distribution with changelog entries through 2026-08-26; Release 5 initially appeared 2025-09-28. No original archive, texture, DLL, PDB, asset bundle, shader binary, or extracted copyrighted code has been committed to Orbit. The user said textures are missing; in this archive the cloud configuration references external Texture/PluginData files not present, so clouds cannot be reconstructed or judged visually from this distribution alone.

Archive inventory: 87 ZIP entries (including directories), approximately 66.7 MB uncompressed and 65.3 MB compressed. Relevant groups:
- GameData/EnvironmentalVisualEnhancements/Plugins/*.dll and *.pdb: managed plug-in binaries / symbols, not editable source for the current cloud shaders.
- GameData/EnvironmentalVisualEnhancements/eveshaders.bundle: compiled Unity shader assets, not text shader source.
- GameData/EnvironmentalVisualEnhancements/stbn.R8 and stbn_unitvec3.ARGB32: bundled stochastic/noise data. Package states that STBN is licensed to NVIDIA.
- GameData/Scatterer/Scatterer.dll and shaders/scatterershaders-directx and scatterershaders-opengl: compiled integration and shader bundles.
- GameData/StockVolumetricClouds/Clouds/clouds.cfg: 3,594-line authored cloud configurations including Release 5 nodes.
- GameData/StockVolumetricClouds/raymarchedClouds.cfg: global light-volume settings.
- GameData/StockVolumetricClouds/Clouds/{droplets,lightning,particleFields,wetSurfaces}.cfg: linked effects.
- GameData/Scatterer/config/{config,qualityPresets}.cfg and StockScattererConfigs/Planets/*/atmo.cfg: atmosphere/render-quality setup.

Legal provenance: the supplied License.txt says the preview volumetric-cloud build is All rights reserved, and it identifies separate licensors for STBN and lightning sounds. This document analyses observed configuration and public descriptions, but does not redistribute third-party implementation, reverse-engineered shader code, or binary-derived source. Older public LGhassen/EnvironmentalVisualEnhancements source is a legacy codebase: do not treat its historical Atmosphere/CloudsVolume.cs or old shaders as proof of the Release 5 raymarch code. The public Scatterer source states that shader versions later than 0.0772 are not included there. If exact Release 5 shader implementation is needed, obtain authoritative source or developer permission.

Evidence notation:
- [A] Observed directly in user-supplied archive config, changelog, file inventory.
- [B] Documented by official EVE wiki or blackrack release notes.
- [C] Orbit-specific proposal or engineering inference; NOT confirmed to be blackrack internals.
Numerical configuration values are authored defaults for this package and not measured GPU costs.

# Release 5 rendering decomposition

1. Global atmospheric cloud authoring. Each Cloud OBJECT selects a body and layer. Coverage, type and sometimes colour are mapped across the body; optional speed/flow and painter-authored maps control variation. [A,B]
2. Detailed raymarched layers. A layerRaymarchedVolumeV5 node defines cloud types, height ranges, density, shape noise, step schedule, phase functions and effects. [A]
3. Independent long-distance cloud light volume. Shared by participating layers; stores long-range light and ambient terms, updated over multiple frames. Short local light marches can remain for near-field detail. [A,B]
4. Temporal reconstruction. Only a fraction of desired cloud pixels are raymarched per frame; history reprojects the missing samples and converges toward a complete image. [B]
5. Layer compositing. Release 5 allows overlapping volumetric altitude ranges by rendering layers in separate passes with an explicitly selected render order. Cross-layer shadowing comes through the light volume; geometric occlusion between overlapping layers is approximate. [A,B]
6. Scatterer/Deferred integration. Cloud shading receives atmospheric sunlight/sky; cloud shadows affect lighting instead of being applied as a final-screen dark filter. Reflection probes include clouds, and atmosphere scattering responds to cloud shadows for god rays. [A,B]
7. Orbital representation. Distant 2D cloud textures, normal maps, and generated 2D appearance are blended as volumetric features become undersampled; scaledFadeStartAltitude/EndAltitude control this in configs. [A,B]

IMPORTANT: these are functional components established by public documentation and configuration. The exact storage formats, compute dispatch layouts, reprojection validation code, and shader instructions are not recoverable from this archive as readable source.

# Configuration and algorithms, in depth

## 1. Cloud field and authoring

The 80,458-byte clouds.cfg contains 19 occurrences of the layerRaymarchedVolumeV5 node and around 70 per-type declarations. Each per-type Item may include typeName, minAltitude, maxAltitude, interpolateCloudHeights, density, baseNoiseTiling, ambientVolume, noiseEdgeHardness, dropletsDensity, wetSurfacesIntensity, lightningFrequency, particleFieldDensity, coverageCurve and densityCurve. Its cloud families include stratus, cumulus, congestus, thunder, fog, rain, snow, dust, plume/volcano-oriented fields, and high thin cloud types.

CoverageCurve and densityCurve solve different problems. Coverage can govern geographical occurrence/occupation, while a density-vs-height profile reshapes extinction inside the participating column. They should not be conflated. The actual config lists per-type densities from 0 (some utility effects) to around 0.2 in its native parameter units; do not assume those are Orbit SI extinction values without examining the proprietary renderer's scaling.

The painter's tiles are authored intermediate-scale cloud formation maps, rather than runtime procedural tile meshes. They write coverage/type data to the global maps, optionally modulated by painter masks; the upstream wiki explicitly says tiles are not sampled at runtime as added cloud detail. This is important for Orbit's performance and authoring design: placement can be complex offline and remain a cheap field lookup at runtime. Archive entries tileName, size, remapCoverage and remapType match this model. [A,B]

The archive lacks the referenced bulk cloud textures, so no assertion can be made about their actual waveforms, spatial resolution, or appearance. Map references are evidence of the design contract, not the pixels.

## 2. Release 5 noise shaping

The changelog specifically describes deriving more visible detail from a single baseNoise texture; adjustable persistence, lacunarity, erosion depth and round/billowy shaping; and improvements to noise detiling. The configuration includes per-layer noise/erosionDepth and worley/spherical and worley/persistence, plus type-specific noiseEdgeHardness. [A]

Interpretation [C]: a useful transferable architecture is one tiled base 3D field with carefully decorrelated coordinate warps and per-type morphological remapping instead of expensive independent 3D octave fields for every cloud component. This does NOT establish the precise noise sample sequence used by EVE Release 5.

Upstream docs say noise detiling is visually useful and performance-intensive. Moving all noise detiling into every secondary lighting integration is not advisable without an A/B measurement. Use coarse/filtered density for secondary optical-depth integration, high-detail erosion near visible silhouettes. [B,C]

## 3. March controls and layered detail

Among the 18 explicitly authored baseStepSize entries are 75, 120, 150, 300, 500, 750, and much larger sentinel/disabled-path values. adaptiveStepSizeFactor typically lies around 0.0025-0.01; maxStepSize often 360-3000. Some layers set lightMarchSteps=0 and distance=0, while others set 4, 8 or 16 local light steps and finite lightMarchDistance. [A]

Do not equate these settings with exact step counts executed per frame; early outs, density gates, and current camera geometry determine real work. Very large configured steps must not be averaged into physically meaningful per-layer ray distances.

The Release 4 light-volume docs advise retaining local light marching for fine detail while removing long-range steps that a background cache can provide. This explains why some Release 5 configs have zero short-range Sun steps yet can still be lit and shadowed. [B]

## 4. Background sun/ambient light volume

The distribution has global configuration values horizontalResolution=224, directLightTimeSlicing=12, maxTimewarpUpdateRateIncrease=3. [A] The official light-volume wiki says:
- One light-volume solution is shared by participating layers.
- Horizontal distribution prioritizes the camera and reaches the visible horizon; it recenters based on camera altitude and movement.
- The vertical range spans enabled layer altitudes.
- Sun shadows and skylight ambient have independent update rates; ambient can update less frequently.
- A timewarp multiplier accelerates updates to avoid stale lighting. [B]

The config does not specify all dimensions or the exact voxel buffer format, so do not infer a 224x224xN texture or a specific GPU byte count. The directLightTimeSlicing=12 setting means 12-frame amortization by configuration, not a guarantee that any specific voxel receives precisely one update each 12 frames.

This is the closest conceptual match to Orbit's CloudRenderer::LightVolume and UpdateLightVolume. The distinguishing design recommendation is to make the primary march preferentially use cached long-distance optical depth while a small correction handles billow-scale detail, subject to error/validity checks.

## 5. Temporal upscaling and history

The official wiki calls temporal upscaling EVE's main optimization. It describes 8x/9x as quality/performance sweet spots for older documented builds, constructing a fuller image from different per-frame sample subsets with motion reprojection. These are HISTORICAL recommendations and not necessarily optimal for this Release 5 distribution or for Orbit's shaders. [B]

The Release 5 patch history includes specific fixes for:
- Thin fog in front of denser distant clouds appearing pixelated.
- Overlapping layers smearing at large distance.
- Trailing artifacts during fast camera motion.
- TAA flickering on orbital vessels.
- Previous orbital representation and cloud noise aliasing.

These failures are consistent with needing robust history depth/ordering, multiple participating layers, velocity/advected density, disocclusion and camera-motion guards. The exact weighting/clamping algorithm is not in the archive; the preceding statement is a recommended design inference, not an EVE reverse-engineering claim. [A,C]

An independent 9x sample pattern does not reduce shader cost by exactly nine times; resolve work, cache updates, geometry, divergence and changed camera paths remain. Measure whole-frame time as well as the march pass.

## 6. Overlapping layers

Release 5 deliberately keeps separate draw passes for overlapping cloud layers because a single combined volume would be expensive. The official wiki says cross-layer occlusion is approximate and recommends drawing denser layers before thinner ones, manually setting overlapRenderOrder to distinct values. It lists a sensible order: dense main deck, cumulonimbus, then fog. [B] Archive configs include overlapRenderOrder values 2 and 3 where needed. [A]

A correct physical front-to-back radiance integration for arbitrary overlapping media would mix extinction of every material at every step. Per-layer compositing is cheaper, but if the ranges overlap, a single order is not globally correct for every viewing ray. That is the quality trade-off; do not simply copy this limitation into Orbit without an explicit quality mode.

For Orbit's weather solver, propose: keep separate layer marching where disjoint; use ordered separate passes for distant/low-impact overlaps; optionally merge density into a local sparse multi-type volume for complicated storm intersections within a camera-near region. This is a proposal [C], not a documented blackrack design.

## 7. SDF empty-space skipping: an important negative result

The official EVE signed-distance wiki explains its optional SDF represents EMPTY COVERAGE-MAP regions; it cannot skip empty space above a cloud inside a column. It warns that performance gains are often small and can be negative, except for sparse/tall layers or very fine step sizes. Archive configs include five sdfMap references, including desert/dust and volcanic/cloud structures. [A,B]

Implication: EVE's SDF should not be described as an exact 3D cloud-surface SDF or a universal guaranteed speedup. For Orbit, first benchmark conservative coverage-distance skipping, then consider a real hierarchical 3D opacity bound if its update budget is justified.

## 8. Integrated atmosphere, shadows, reflection probes

The supplied Scatterer config enables integrateWithEVEClouds, sunlightExtinction, eclipses, rings, cloud god rays, and disabled terrain raymarched god rays at the supplied quality state. raymarchedGodraysStepCount=50. Quality presets enable expensive effects at higher levels. [A]

Release 5 changed cloud shadows from post-darkening overlays to lighting-stage attenuation and made clouds visible to reflection probes. Scatterer uses a shared atmospheric scattering model while EVE owns the cloud density. This reduces the risk of visibly incompatible lighting across environment surfaces. [B]

Orbit should propagate clear contracts for direct irradiance, atmosphere transmission, sky radiance, cloud extinction, source transmittance, cloud-shadow visibility, and surface/reflective consumers, with no double application of cloud shadows. See /rendering/clouds/references/blackrack-eve-orbit-design.

## 9. Ground to orbit and camera-space compatibility

The changelog mentions generation of 2D color/normal maps from volumetrics, 2D cloud normal maps, fades between orbital and volume representations, camera and far-planet depth based scattering, and ordering atmospheres when one planet lies behind another. [A,B]

For Orbit: preserve the same underlying cloud weather field and consistent optical-depth statistics across near/far representations. At orbit distance, an image-based cloud layer is an approximation; near-camera turbulence and self-shadowing should be resolved with the volumetric renderer. Crossfade must preserve radiance and approximate transmittance, not simply alpha-blend two independently lit renderings.

## 10. Timewarp, storms, and noncloud subsystems

The Release 5 archive includes sound, precipitation, droplets, wet surfaces, lightning, particle fields and eclipse-caster configs. These are not core cloud march algorithms; they demonstrate a complete weather/lighting presentation stack with separate update domains. With timewarp, update physical weather, dense storm animation, cache refresh, and visual reprojection at distinct rates. A fixed 12-frame cadence alone would not handle enormous simulation-time jumps correctly; use generation/version validation.

# Reproducibility and interpretation

Use this file as a reference map, not an implementation spec inferred from closed binaries. The supplied package is binary-only with missing cloud textures, and was not executed under KSP or profiled on the user's GPU. All claims are attributable to (A) configuration/changelog or (B) public project documentation. Engineering designs live in the sibling Orbit document.

## Public references

- Blackrack Release 5 notes (2025-09-28, updated 2026-08): https://www.patreon.com/blackrack/posts/true-volumetric-139879553
- EVE documentation index: https://github.com/LGhassen/EnvironmentalVisualEnhancements/wiki
- Release 5 new features: https://github.com/LGhassen/EnvironmentalVisualEnhancements/wiki/Release-5-additions-(28%E2%80%9009%E2%80%902025)
- Light volume: https://github.com/LGhassen/EnvironmentalVisualEnhancements/wiki/Light-volume-(Release-4)
- Temporal upscaling: https://github.com/LGhassen/EnvironmentalVisualEnhancements/wiki/Temporal-upscaling-and-noise-detiling
- Overlapping layers: https://github.com/LGhassen/EnvironmentalVisualEnhancements/wiki/Overlapping-layers(Release-5)
- SDF warning: https://github.com/LGhassen/EnvironmentalVisualEnhancements/wiki/Signed-Distance-Fields-(Release-4)
- Painter tiles: https://github.com/LGhassen/EnvironmentalVisualEnhancements/wiki/Painter-tiles-(Release-5)
- EVE public legacy source: https://github.com/LGhassen/EnvironmentalVisualEnhancements
- Scatterer legacy public source, shader availability caveat: https://github.com/LGhassen/Scatterer
