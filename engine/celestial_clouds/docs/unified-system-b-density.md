+++
path = "/rendering/clouds/unified-system-b-density"
title = "Unified Cloud System B2: multi-layer and storm extinction"
kind = "concept"
status = "experimental"
owner_module = "OrbitCelestialClouds"
summary = "B2 code contracts for shared weather inputs, independent layered volumetric extinction, nested GPU storm sampling and shadow parity."
keywords = ["clouds", "unified B", "storm", "density", "layers", "GPU ABI", "visibility"]
sources = ["engine/celestial_clouds/src/UnifiedCloudRenderer.cpp", "engine/celestial_clouds/src/CloudField.cpp", "engine/celestial_clouds/include/orbit/celestial_clouds/CloudField.hpp", "engine/studio_ui/src/StudioViewportStorm.cpp", "engine/studio_ui/src/StudioViewportLightingPass.cpp", "engine/studio_ui/src/StudioViewportAtmospherePass.cpp", "engine/celestial_clouds/tests/UnifiedCloudFoundationTests.cpp"]
related = ["/rendering/clouds", "/rendering/clouds/unified-system-b", "/rendering/clouds/references/blackrack-eve-orbit-design"]
depends_on = ["/rendering/clouds/unified-system-b"]
[routes]
"how B integrates overlapping cloud layers and storms" = "/rendering/clouds/unified-system-b-density"
"what B2 still needs before valid A/B benchmarking" = "/rendering/clouds/unified-system-b-density"
+++

# B2 integration status

Code committed to 0.1.1-Harmony; compilation and hardware validation NOT yet performed this session. A remains the default. The System B selector is an experimental preview and does not establish quality or runtime equivalence.

# Input ownership and GPU ABI

B reads the same immutable CloudFieldProduct and GpuCloudFieldProduct as A. It does not call the A render shader or A cache. The GPU cloud field keeps its existing 16-byte weather texels at precisely the same offsets; a 32-byte GpuCloudLayerDescriptor is APPENDED after all texels for every authored layer. Weather texels pack raw weather, cirrus coverage, precipitation and cloudType. The appended descriptors are two float4 records: (baseAltitudeMeters, topAltitudeMeters, peakOpticalDepth, coverageBias) and (densityExponent, singleScatteringAlbedo, anisotropy, shadowParticipation). EncodeGpuCloudLayer is a pure C++ function also used in unit tests. A's existing byte indexing is unchanged.

For N layers and face size R, the descriptor tail begins at byte N*6*R*R*16. Layer i begins at tail+i*32. No second GPU weather-map allocation is required per viewport.

# Unified extinction model

B loops over each layer, checks its own real altitude bounds, samples the correct weather cubemap face, evaluates type-dependent profile (stratus/cumulus/tower plus upper cirrus contribution), and integrates total extinction as a physical sum. Overlapping cloud layers must not overwrite each other. All layers contribute to the same optical-depth accumulation. Scattering albedo and anisotropy are tracked as extinction-weighted optical coefficients. A 64-step initial view march and a 12-step receiver-shadow pass both use B's exact same Density function, though their numerical integration resolutions differ.

B uses a single live storm export from the existing GpuStormSolver or NestedGpuSolver. StudioViewportRenderer::PublishStormCloudExport publishes that buffer to BOTH renderers without stepping the solver twice. B owns a separate zero-active dummy when no storm exists; it never reads A's dummy, history, volume light cache or shader implementation. The B shader independently decodes the existing 32-float storm header and parent/optional child voxel grids. Interpolation is trilinear, with the nested region blended by its edge sponge. StormInfluence provides the 2D mask and the domain-edge fade applies the lifecycle intensity.

A convective procedural source is REPLACED rather than simply added inside the storm influence mask: sigma_total = independent_stratus_fog_cirrus + lerp(procedural_convective, simulated_storm, mask). This avoids double opacity, preserves unrelated layers, and returns to procedural coverage beyond the storm. CPU packing and the GPU sampling convention must be updated together whenever the storm-export schema changes.

The GPU shell's upper bound expands to 21 km while the current 40 x 500 m storm domain is published, preventing an overshooting top from being clipped by a 6 km authored procedural layer. This conservative bound must later be replaced with export-defined nz*dz once variable-altitude storms are supported.

# Cross-system constraints and open risks

- B uses its own March, ReceiverShadow and Composite render-graph passes and distinct persistent targets under the UnifiedB prefix; A retains its unchanged shader and output.
- B's shadow target feeds the common direct-lighting consumer instead of multiplying A+B shadow factors.
- The Sun/camera/body frame comes from the same existing authoritative Studio physics state, not a second simulation.
- Receiver shadows respect layer shadowParticipation; the shared Density function ensures storm and procedural geometry are consistent.
- B2 does not implement A's synthetic Cloud Lab override. When the lab is enabled, B2 and its shadow are explicitly suppressed and the editor displays a warning; it must not render unrelated weather as a claimed valid B image.
- Studio graph composition adds CloudShadow before Atmosphere; storm export is updated during atmosphere composition, before GPU callback recording. B's callbacks read the published storm pointer when recording, avoiding a second physics step. Multi-view cross-planet publication and pinning the buffer until GPU fences retire remains OPEN and must be solved before declaring full B compatibility.
- The existing storm exporter must transition its GPU buffer to readable state before both renderers consume it. Verify this with Vulkan validation.
- The direct Sun, air, terrain relief and eclipse models are NOT unified with B2's simplified lighting. B3 needs a distinct optical-depth/sunlight cache, finite-disc occlusion and a physically coherent atmosphere transport contract.
- B2 still uses fixed step budgets, modest procedural shape, no fine storm erosion/rain shafts, no point-light lightning, no temporal reconstruction, no multi-frequency sky illumination and no independent per-B global weather simulation. B2 is a density plumbing milestone, not a visual-quality claim.

# Mandatory checks

1. python tools/check_unified_cloud_shaders.py
2. cmake --build build --config Debug --target OrbitUnifiedCloudFoundationTests OrbitStudio
3. ctest --test-dir build -C Debug -R Orbit.UnifiedCloudFoundation --output-on-failure
4. Compare A reference versus B2 preview with two nonoverlapping layers, thin cirrus overlapping cumulonimbus, and a nested simulated storm.
5. Switch the storm off/on and verify spatial blending, procedural restoration, no memory/state leak or flicker.
6. Inspect clouds from below base, inside storm, at 20+ km, at orbit, and at grazing sunrise. Capture viewport and receiver-shadow image tests separately.
7. GPU profiler: compare matching scenarios and resolutions using the independent UnifiedB.March, UnifiedB.ReceiverShadow and UnifiedB.Composite passes, including whole-frame time.
8. Revert to A and ensure its pixels match the prior baseline within capture noise.

Read /rendering/clouds/unified-system-b for the broader A/B architecture and unresolved system-integration conflict register.
