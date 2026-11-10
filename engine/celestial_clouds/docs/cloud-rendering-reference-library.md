+++
path = "/rendering/clouds/references/library"
title = "Realtime volumetric cloud rendering reference library"
kind = "reference"
status = "research"
owner_module = "OrbitCelestialClouds"
summary = "Curated source-ranked papers, real-time renderer implementations and vendor technical references, connected to specific Orbit code and decisions."
keywords = ["cloud references", "EVE", "Scatterer", "blackrack", "Nubis", "temporal", "volumetric", "atmosphere", "optimization"]
sources = ["engine/celestial_clouds/docs/blackrack-eve-release5-reference.md", "engine/celestial_clouds/docs/blackrack-eve-orbit-design.md", "engine/celestial_clouds/src/CloudRenderer.cpp", "engine/celestial_clouds/docs/raymarch.md"]
related = ["/rendering/clouds", "/rendering/clouds/references/blackrack-eve-release5", "/rendering/clouds/references/blackrack-eve-orbit-design"]
depends_on = ["/rendering/clouds"]
+++

# Scope and source classes

These sources form a technical reading route, not software dependencies. Do not vendor materials without checking their licences. Blackrack Release 5 supplied binary distribution is All rights reserved and is NOT stored in Orbit. It contains shaders only as compiled Unity bundles, plus plugin DLLs and configurations, not editable current volumetric shader source. EVE GitHub public repo contains older EVE code; Scatterer explicitly states newer shaders are not included in its repository. Never present either legacy tree as the Release 5 source of truth.

Each entry lists a use case and its status:
- Direct: techniques explicitly documented by their authors.
- Code: implementation available for independent inspection, subject to that repository's license.
- Research: published paper / production presentation; not plug-and-play code.
- Archive: user-supplied configuration and change log; not independently executable here due to absent cloud textures.

# First-pass reading order

1. **EVE Release 5 (primary example)**: blackrack release notes, official EVE wiki, inspected user archive. Read /rendering/clouds/references/blackrack-eve-release5 first, then the code map at /rendering/clouds/references/blackrack-eve-orbit-design.
2. **Guerrilla Nubis and Nubis Evolved / Nubis3**: sparse occupancy, traversal, authored cloud fields and high-frequency shape.
3. **Unreal volumetric-cloud docs**: Beer shadow-map versus secondary marching, phase/sky lighting and resource trade-offs.
4. **Hillaire sky and atmosphere**: atmosphere LUTs and consistent radiometric transmittance at orbital scales.
5. **Vulkan code references (Meteoros and experimental voxel clouds)**: learn the practical binding and filtering choices, after reviewing licences.

# Primary Release 5 and EVE material

- Release 5 original announcement and changelog, 2025-09-28 with updates through 2026-08: https://www.patreon.com/blackrack/posts/true-volumetric-139879553 . Direct evidence of new noise/detail shaping, painter workflow, shadow integration, reflections and orbital representation. Some media/downloads are access restricted.
- EVE wiki index: https://github.com/LGhassen/EnvironmentalVisualEnhancements/wiki . Official navigation to configuration and release-specific changes.
- EVE Release 5 additions: https://github.com/LGhassen/EnvironmentalVisualEnhancements/wiki/Release-5-additions-(28%E2%80%9009%E2%80%902025) . New density curves, noise hardness, painter tiles and overlap.
- Light volume: https://github.com/LGhassen/EnvironmentalVisualEnhancements/wiki/Light-volume-(Release-4) . Most relevant to the high measured Orbit cloud cost: shared long-range shadows and ambient updated over separate frame schedules. Short local march is retained for fine detail.
- Temporal upscaling: https://github.com/LGhassen/EnvironmentalVisualEnhancements/wiki/Temporal-upscaling-and-noise-detiling . Explains staggered pixel shading and reprojection, with historical 8x/9x recommended quality trade-offs. Not a quality or speed guarantee for current Orbit.
- Layer overlap: https://github.com/LGhassen/EnvironmentalVisualEnhancements/wiki/Overlapping-layers(Release-5) . Cross-layer shadows via volume; separate passes with approximate occlusion and denser-first order. Do not mistake this for physically exact volume merging.
- Signed-distance field: https://github.com/LGhassen/EnvironmentalVisualEnhancements/wiki/Signed-Distance-Fields-(Release-4) . Explicitly warns that SDF skipping is restricted to coverage-map emptiness and often yields limited/negative speedup.
- Painter tiles: https://github.com/LGhassen/EnvironmentalVisualEnhancements/wiki/Painter-tiles-(Release-5) . Intermediate-scale formation motifs are AUTHORING inputs to coverage/type maps, not runtime 3D tile detail.
- Raymarched config reference: https://github.com/LGhassen/EnvironmentalVisualEnhancements/wiki/Raymarched-cloud-configuration . Authoring density, layers, light steps, phase functions, maps.
- Scatterer atmospheric integration docs: https://github.com/LGhassen/Scatterer/wiki/GeneralConfig and https://github.com/LGhassen/Scatterer/wiki/PlanetsConfig . Sunlight extinction, planet scattering, terrain god rays, EVE integrations.

# Related renderer research

- Guerrilla Games: Nubis: Authoring Real-Time Volumetric Cloudscapes with the Decima Engine, 2017. https://www.guerrilla-games.com/read/nubis-authoring-real-time-volumetric-cloudscapes-with-the-decima-engine . Research. Cloudscape density representation, authorability, noise, atmosphere coupling. Do not use the published PS4 render-time numbers as an Orbit performance promise.
- Guerrilla Games: Nubis, Evolved, 2022. https://www.guerrilla-games.com/read/nubis-evolved . Research. Traversable clouds, quality and movement.
- Guerrilla Games: Nubis Cubed, 2023. https://www.guerrilla-games.com/read/nubis-cubed . Research. SDF-like and voxel-based traversal acceleration. Important difference: occupancy/SDF benefits depend on material sparsity and update cost.
- Epic Games: Volumetric Cloud Component. https://dev.epicgames.com/documentation/unreal-engine/volumetric-cloud-component-in-unreal-engine . Direct. Distinguishes secondary light marches and Beer shadow map approximations.
- Sebastien Hillaire: Scalable and Production-Ready Sky and Atmosphere Rendering, 2020. https://sebh.github.io/publications/egsr2020.pdf . Research. Atmosphere extinction and scattering LUT foundations; separate optical depth from planet/terrain blockage.
- Meteoros Vulkan cloud renderer. https://github.com/AmanSachan1/Meteoros . Code. Vulkan raymarch, temporal reprojection and lighting experiments; check exact code/licence before reuse.
- Aerkanis Vulkan/Slang Nubis-inspired experiment. https://github.com/KonaBaker/Aerkanis . Code. Volumetric cloud proof of concept; not evidence of production robustness.

# Direct mapping to Orbit documentation and source

- Cloud marching and shading constants: /rendering/clouds/raymarch and engine/celestial_clouds/src/CloudRenderer.cpp.
- Shadow/light-volume technical contract: /rendering/clouds/shadows-and-light-volume and tests/CloudLightVolumeTests.cpp.
- Global weather and cloud types: /rendering/clouds and engine/celestial_clouds/src/CloudField.cpp.
- Near/far representation: engine/celestial_clouds/src/CloudFarField.cpp and engine/studio_ui/src/StudioViewportAtmospherePass.cpp.
- Shared terrain visibility: /rendering/lighting/terrain-sun-visibility.
- Scene-controlled repeatable testing: tools/orbit_ab.py, tools/orbit_report_rendering.py, tools/check_cloud_shaders.py.
- User-supplied benchmark baseline: cloud march ~20.56 ms mean in a specific 320-sample GPU trace, NOT a universal performance cost.

# Research questions still unresolved

- What is the exact shader internal noise lookup and update layout of EVE Release 5? Not disclosed in editable sources in supplied archive. Do not infer exact code from parameters alone.
- How does Release 5 implement depth-aware history rejection for overlapping fog and clouds? Changelog identifies artifacts/fixes, but not exact mathematical rules.
- What fraction of Orbit's measured cloud pass is spent in AltitudeSunTau, primary density noise, aerial perspective or sampling/fill? Requires A/B GPU timing with shader defines.
- Would hierarchy, SDF or checkerboard improve Orbit's specific storm workload? Requires identical-scene measurements and visual comparisons.
- How to make a genuinely unified planetary weather medium across clouds, precipitation and aerosols without overallocating VRAM? Need a local sparse/material representation and conservative streaming budget.

The next engineering milestone should be a measured shader ablation matrix, not immediate large-scale renderer replacement.
