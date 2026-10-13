+++
path = "/rendering/lighting/terrain-sun-visibility"
title = "Shared terrain stellar visibility"
kind = "concept"
status = "stable"
summary = "A body-fixed heightfield derived from TerrainSource supplies the same finite-disc terrain visibility to surface lighting, atmospheric direct scattering and clouds. It covers offscreen mountains and is reused across simulation-time changes."
owner_module = "OrbitLighting"
keywords = ["terrain sun", "mountain shadow", "sun occlusion", "penumbra", "terminator", "reports", "TerrainSunShadow"]
sources = ["engine/lighting/src/TerrainSunShadow.cpp", "engine/lighting/include/orbit/lighting/TerrainSunShadow.hpp", "engine/lighting/include/orbit/lighting/SunVisibilityShader.hpp", "engine/studio_ui/src/StudioViewportTerrainSunShadow.cpp", "engine/lighting/src/DirectLighting.cpp", "tests/TerrainSunShadowGpuTests.cpp", "tools/orbit_report_rendering.py"]
symbols = ["TerrainSunShadowField", "BuildTerrainSunShadowField", "TerrainSunVisibility", "TerrainSunReliefVisibility", "AtmosphereStellarTransmittance"]
invariants = [
  "TerrainSource, radius and source revision are authoritative; the 1024x512 heightfield is a disposable derived product. Camera, screen depth, render LOD and sun direction are absent from its key.",
  "The field samples opaque ground elevation, with longitude wrapping and bilinear interpolation. Standing water is not raised into an opaque mountain caster.",
  "CPU and GPU traverse at half the field spacing, with a 512-sample safety cap. A maximum-relief sphere rejects fully sunlit queries before height reads; finite stellar disc area provides penumbra, not a hard depth comparison.",
  "The normal direct-light path uses this field when ready. The existing sun-audit methods retain their explicit comparison/debug behavior.",
  "Air interpolates extra terrain attenuation separately from the analytic planet eclipse. Cloud light-volume voxels cache extra terrain attenuation beside optical depth; the planetary disc remains analytic at every scattering point. Unavailable cache data uses the shared query.",
  "Candidate fields build asynchronously and activate only for the matching body/source/revision/radius. Render graph closures pin buffers used by commands; an implementation edit uses the central Studio-generation handoff and a failed candidate leaves the running Studio usable."
]
related = ["/rendering/atmosphere", "/rendering/clouds/shadows-and-light-volume", "/rendering/lighting/visibility-and-reflections"]
verify = ["ctest -R Orbit.TerrainSunShadow: CPU ridges, offscreen occlusion, longitude seams and finite-disc partial visibility.", "ctest -R Orbit.TerrainSunShadowGpu: real Vulkan shader versus the CPU reference across umbra, penumbra and full sunlight.", "python tools/orbit_report_rendering.py --reports 10 11 12 13 --out build/report-fixes/verified: replay saved report scenes with fixed exposure and repeat/reference controls."]
verified = "283b7b066432087a3acd420afc2e112b30ca3482"
+++

Terrain resolution bounds the smallest represented caster. The GPU/CPU parity
test verifies traversal of that field; it does not claim sub-texel terrain is
exact. Cloud cascade interpolation is a lighting approximation, while the
uncached shared query remains the reference for terrain visibility.

Atmospheric extinction uses a horizon-relative transmittance lookup inspired by
[Bruneton's separation of transmittance and ground intersections](https://ebruneton.github.io/precomputed_atmospheric_scattering/atmosphere/functions.glsl.html).
Finite-disc visibility is applied separately; ground-blocked zeros must not be
bilinearly blended into extinction near the terminator.
