+++
path = "/rendering/clouds/reference-eve-v5"
title = "EVE V5 / Scatterer frozen reference integration specification"
kind = "subsystem"
status = "proposed"
owner_module = "OrbitCelestialClouds"
summary = "Source-constrained technical blueprint covering terrain, atmosphere, cloud rendering, oceans, weather, effects and A/B verification."
keywords = ["EVE V5", "blackrack", "scatterer", "reference", "cloud renderer", "terrain", "atmosphere"]
sources = ["engine/celestial_clouds/src/UnifiedCloudRenderer.cpp", "engine/studio_ui/src/StudioViewportAtmospherePass.cpp", "engine/studio_ui/src/StudioViewportLightingPass.cpp", "engine/celestial_atmosphere/include/orbit/celestial_atmosphere/AtmosphereRenderer.hpp", "engine/lighting/include/orbit/lighting/DirectLighting.hpp"]
related = ["/rendering/clouds", "/rendering/clouds/unified-system-b"]
[routes]
"complete EVE V5 reference integration spec" = "/rendering/clouds/reference-eve-v5"
"blackrack renderer integration plan" = "/rendering/clouds/reference-eve-v5"
+++

# Authority, goal, essential limitation

The supplied artifact is RaymarchedVolumetricsEarlyAccess23_08_26(1).zip; verified SHA-256:
`d4c74123c76651969bc81073e3e6eb2e8236ae464a336618fd9baf8baebc42a7`.
Its bundled changelog ends on 23 August 2026. It is a **compiled Unity/KSP release**, NOT a C++/HLSL/C# source tree. It contains EVE and Scatterer DLLs, EVE shader bundle, Scatterer DirectX/OpenGL shader bundles, configs, and blue-noise assets. The ZIP's License.txt states **All rights reserved** for the preview build; the STBN license is credited to NVIDIA and included sound effects to Epidemic Sound. Never upload these binaries, bundled shaders, licensed textures, sample data or audio to this public repository without permission.

A faithful **line-for-line Vulkan port is not available from this ZIP**. Shader source, actual runtime algorithms, Unity pass internals, and referenced original texture pixels are not supplied. We can document exact shipped *configuration* and publicly described behavior, and implement a clean-room original Orbit adapter. Precise implementation parity requires properly licensed original source/shaders and textures, or behaviorally measured golden references from the legal KSP runtime. Do not present plausible math as "extracted original source".

The user's desired baseline must be **frozen and isolated**: leave A (existing CloudRenderer) and B (UnifiedCloudRenderer) untouched until a separate, explicit EveV5Reference backend can be built, validated and compared. Do not replace one weather/storm physics solver with another renderer-owned simulation.

## Evidence labels used in every chapter

- **EXACT-ARCHIVE**: directly observed config paths, parameter values, release notes, dependency names and binary inventory.
- **PUBLIC-SOURCE**: openly published upstream EVE/Scatterer code/wiki; versions and licenses may differ from the privately shipped V5 release.
- **EXISTING-ORBIT**: verified symbols and APIs on Harmony, identified with exact path.
- **PROPOSED-PORT**: independently authored C++/HLSL/Vulkan algorithms, not evidence of identical proprietary implementation.
- **UNRESOLVED**: not recoverable without missing assets, original shader source or controlled runtime measurement.

All source snippets in these documents are new **PROPOSED-PORT** code unless stated otherwise.

## Actual archive inventory

The ZIP has 87 entries (including folders), approximately 66.7 MB uncompressed. Its clouds.cfg defines **19 V5 cloud-layer objects, 70 cloud-type entries, 27 painter tiles** on five planets: Kerbin 6 layers, Duna 3, Eve 2, Jool 4, Laythe 4. The configs refer to **103 distinct texture/sound resource paths** not supplied in this ZIP. These must be validated individually. Any generated replacement is a visibly labeled SYNTHETIC TEST ASSET and cannot prove visual equivalence.

## Ownership and dataflow

    WORLD AUTHORITY [EXISTING-ORBIT]
      Universe/Body + absolute SimulationTime
      TerrainSource + world revisions + oceans
      WeatherModel + CloudField + GPU storm export
      Celestial lighting + Atmosphere LUTs + local light events
                      |
              immutable GPU FrameSnapshot
                /       |        \
             A ref   B exp   EVE_V5_REF
                                |
                     EVE Configuration Adapter
                     2D weather / SDF / flow maps
                     Typed cloud physical medium
                     3D noise + overlap layer policy
                        /           \
                 Light transport   View ray integrator
                 direct sky /      near-to-orbit /
                 cache / shadow    conservative LOD
                        \           /
                     Cloud Reconstruction
                     scene-linear L + T + motion
                                |
                 Common cloud shadow and visibility
                 Atmosphere/terrain/ocean integration
                 HDR postprocess, adaptation, profiler

A/B/EVE outputs must have identical physical units and signatures. Cache/source ownership and lifetimes remain separate for each backend. Rendering passes may be fused physically if it improves performance; conceptual separation is mandatory.

## Chapter index (implementation-facing)

1. [01 Package and reference presets](reference-eve-v5-01-package-and-presets.md): byte-level provenance, all cloud-layer families, V5 configuration knobs, quality presets, missing dependencies.
2. [02 Configs, assets, authoring](reference-eve-v5-02-config-and-assets.md): recursive parser, curves, noise descriptors, channel maps, imported material types, validation, private asset resolver and C++ examples.
3. [03 Physical medium, intersections and LOD](reference-eve-v5-03-medium-and-geometry.md): density/extinction contract, noise, weather, storm blending, shell geometry, SDF, adaptive ray schedule, near/far cloud consistency, code.
4. [04 Atmosphere and light transport](reference-eve-v5-04-atmosphere-and-lighting.md): scattering, sky LUTs, Sun/eclipse/terrain visibility, multiple-scattering and sun-volume, penumbra, god rays, reflection, code.
5. [05 Terrain, ocean and wet surfaces](reference-eve-v5-05-terrain-and-water.md): body depth, terrain/ship shadows, ocean/glass handling, rain-puddle accumulation, surface material updates, shader contracts.
6. [06 Weather FX and events](reference-eve-v5-06-events-and-fx.md): rain/snow/dust, lightning, particle fields, cockpit droplets, audio, event schedules, environment interactions, code.
7. [07 Vulkan render graph](reference-eve-v5-07-render-graph.md): exact Orbit insertion points, graph ordering, resource states and barriers, buffers/descriptors, multi-view and timewarp, code.
8. [08 Verification and migration](reference-eve-v5-08-validation-and-migration.md): exact tests, deterministic golden scenes, optical error metrics, GPU budgets, scoped rollout, blockers and CI.
9. [09 Implementation ledger](reference-eve-v5-09-implementation-ledger.md): complete dependency graph, system/subsystem work items, production/contract freeze gates and code ownership.
10. [10 Per-planet calibration crosswalk](reference-eve-v5-10-profile-crosswalk.md): all ordered V5 cloud types plus authored atmosphere/ocean coefficients and calibration boundaries.

These chapters are designed for implementation with little architectural guessing, but **they are not a substitute for unavailable proprietary source**.

## No-double-count rules

- Terrain, atmosphere, cloud, water and geometry retain distinct physical authority. A render backend does not own their simulation.
- Scene-linear cloud result: (integrated radiance L, transmittance T, optical depth provenance and motion/depth metadata). Compositing is foreground L + foreground T times background L; never multiply T twice.
- Stellar source visibility = planet/ellipsoid/eclipse factor times terrain shadow factor times cloud factor times atmosphere factor, each applied exactly once at its appropriate integration stage. Diffuse skylight is NOT multiplied by direct solar cloud-shadow texture.
- Reference V5 layer ordering, cloudType interpolation, Hermite curves, wind, flow map and 200–260 km orbital fade (for Kerbin) are configuration data. Do not replace these with current coarse A/B cloudType ramp.
- Camera/body transforms are double on CPU and camera-relative on GPU. A near/far representation change cannot move the clouds or alter region mean transmittance.
- Every referenced resource must be present, or the importer must report the precise missing path. A synthetic lab visualization must be explicitly marked as such.
- Generation tokens pin GPU inputs across render-graph execution, and all output caches have stable view/body identity and fence-safe release.
- Existing Orbit editor remains the single authoring UI; add diagnostics under Debug/Profiler and Build Procedurals, not a separate launcher.

## Public documentation (not proof of exact proprietary V5 shaders)

- https://github.com/LGhassen/EnvironmentalVisualEnhancements/wiki/Raymarched-cloud-configuration
- https://github.com/LGhassen/EnvironmentalVisualEnhancements (public older source; mixed per-file licenses)
- https://github.com/LGhassen/Scatterer (public plugin GPL-3.0, later shaders not shipped as source)
- https://github.com/LGhassen/Scatterer/wiki/PlanetsConfig
- https://github.com/LGhassen/EnvironmentalVisualEnhancements/wiki/Temporal-upscaling-and-noise-detiling

**Milestone status:** documentation / independent port specification. No EVE_V5_REF Vulkan code path has been compiled or validated. The ZIP cannot be linked directly into Orbit's Vulkan renderer.
