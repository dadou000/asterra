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
  "CPU and GPU traverse at half the field spacing, with a 512-sample safety cap. A maximum-relief sphere rejects fully sunlit queries before height reads; the minimum horizon slope is accumulated along the ray and its angle and finite stellar-disc integral are evaluated once for both solar limbs.",
  "The normal direct-light path uses this field when ready. The existing sun-audit methods retain their explicit comparison/debug behavior.",
  "Air interpolates extra terrain attenuation separately from the analytic planet eclipse. Cloud light-volume voxels cache extra terrain attenuation beside optical depth; the planetary disc remains analytic at every scattering point. Unavailable cache data uses the shared query.",
  "A single bounded low-priority job worker builds shadow fields without render-thread waits; builds are shared between viewports for the same body/source/radius and old generations may be canceled. Source revisions are checked during generation and before publishing.",
  "Unpublished GPU candidates are populated in bounded 65536-sample chunks per frame; no incomplete buffer is bound to a render graph. GPU resource creation and per-frame buffer mapping remain RHI operations and require GPU stall profiling.",
  "Candidates activate only for the exact body/source/revision/radius. Render graph closures pin published buffers; hot native changes use central Studio-generation handoff, preserving the previous valid generation on failed candidate builds."
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

## Terminator experiment and scheduling limitations (2026-11-10)

The CPU and HLSL paths now obtain a single minimum clearance-to-distance slope
per ray, converting it to a signed horizon angle only after traversal. A uniform stellar disc becomes fully illuminated when the minimum angle
is above its angular radius, fully occluded below its negative angular radius,
and partially visible between those two *solar-limb* envelopes. This is
algebraically equivalent to applying the monotone disc fraction at every ray
sample, but removes expensive repeated inverse-trigonometric disc integrals.
The heightfield remains body-fixed; neither camera movement nor a clipmap
recenter changes the terminator silhouette.

The 1024x512 field is a coarse global caster representation. It cannot reproduce
fine erosion ridges, metre-scale terrain shadows, or cloud volumetric occlusion
by itself. A future local relief hierarchy must share TerrainSource authority
and conservative min/max visibility rather than inventing a second terrain
mesh or a screen-space-only terrain shadow path.

The worker pool bounds CPU concurrency to one background producer per renderer.
The ready future is polled with zero timeout, outdated work may be canceled,
and terrain buffer transfers are throttled to 256 KiB of elevations per frame.
This does **not** prove that CreateBuffer or Map/Unmap are nonblocking on every
RHI/device; only a timeline-fence-backed transfer queue can make that stronger
guarantee. Revisions must never publish partial terrain-shadow buffers.

The existing `verified` field identifies the last previously GPU-validated
revision. This new experiment needs actual Vulkan shader tests, a Studio
frame-time capture, and an erosion-heavy terminator sweep before it can be
marked visually verified.
