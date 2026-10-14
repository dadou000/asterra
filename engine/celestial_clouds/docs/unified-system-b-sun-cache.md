+++
path = "/rendering/clouds/unified-system-b-sun-cache"
title = "Unified Cloud B3: bounded Sun optical-depth cache"
kind = "concept"
status = "experimental"
owner_module = "OrbitCelestialClouds"
summary = "B3 camera-local GPU sunlight extinction cache, generation invalidation, bounded fallbacks, independent pass timing and unresolved transport integration."
keywords = ["unified clouds", "B3", "sunlight cache", "optical depth", "time sliced", "sun volume", "GPU", "timewarp", "A/B"]
sources = ["engine/celestial_clouds/src/UnifiedCloudRenderer.cpp", "engine/celestial_clouds/include/orbit/celestial_clouds/UnifiedCloudRenderer.hpp", "engine/studio_ui/src/StudioViewportAtmospherePass.cpp", "engine/studio_ui/include/orbit/studio_ui/StudioViewportRenderer.hpp", "tools/check_unified_cloud_shaders.py"]
related = ["/rendering/clouds/unified-system-b", "/rendering/clouds/unified-system-b-density", "/rendering/clouds/references/blackrack-eve-orbit-design"]
depends_on = ["/rendering/clouds/unified-system-b-density"]
[routes]
"how B3 sunlight cache works and when it invalidates" = "/rendering/clouds/unified-system-b-sun-cache"
"how to disable B3 and benchmark cloud lighting" = "/rendering/clouds/unified-system-b-sun-cache"
+++

# Status and scope

B3 stage 1 is committed to Harmony. No DXC shader compilation, local Windows build or physical Vulkan run was possible in this session. It should be treated as experimental code until a Studio build and profiler capture succeed. A is unchanged/default. The user previously confirmed B1 ran; this does not validate B3. Neither EVE's proprietary shaders nor A's cloud-light volume code were copied into B.

**B3 is an independent GPU light-cache prototype**, not the final unified cloud/atmosphere/ground visibility solution. It caches optical depth toward the Sun for local cloud lighting; the B receiver-ground-shadow pass still uses its existing direct integration, and the atmosphere, finite solar disc, terrain-horizon visibility, aerial perspective, reflections and skylight have not yet been migrated to this cache. Calling this a unified sunlight/ambient system would be premature.

# Rendering equation and cache meaning

Cloud direct-source transmittance is T_sun(P)=exp(-tau_sun(P)), where tau_sun(P)=integral sigma_t(Q) ds along the light ray until the cloud volume ends. The integral uses the SAME B2 multi-layer/storm Density() representation as the view march. SunDepthDirect clips the ray to the cloud shell and conservatively blocks rays that cross the solid planet. Optical depth is clamped to 24 for numerical stability (transmittance roughly 3.8e-11 at the cap).

B1/B2 used an unoccluded view-to-Sun brightness approximation. B3's view-ray march now multiplies direct cloud scattering by exp(-tau_sun), so clouds can shade other clouds. This is **cloud self-shadowing**, not an atmospheric/terrain shadow completeness claim.

Cached optical depth is interpolated as tau then exponentiated; interpolating already-exponentiated transmittance would have different error properties. The cache does not store tone-mapped RGB and is independent of eye exposure and the final scene target.

# B3 provisional skylight correction (2026-10-14)

The first successful System B screenshot showed an excessively uniform grey cloud blanket. A source audit found two lighting errors in UnifiedCloudRenderer.cpp, unrelated to whether its Sun cache was valid:

- The old ambient source (0.13 + 0.20 * skyFacing) was emitted even on the planetary nightside, without solar daylight gating or any optical-depth reduction within cloud interiors.
- The single-scattering Henyey-Greenstein phase was missing the 1/(4*pi) solid-angle normalization, over-amplifying directional scattering by approximately 12.566.

The new direct source calculates the solar disc's visibility relative to the geometric horizon of the reference planet from the cloud's current altitude. For body radius R and sample radius r, the horizon cosine is -sqrt(max(0, 1-(R/r)^2)). A finite solar angular radius is taken from AtmosphereRenderView::sunAngularRadiusRadians. Direct scattering is proportional to the actual daylit fraction, stellar irradiance, the normalized HG phase and exp(-tauSun). This avoids sunlight being added to the night side simply because the previous skyFacing mapping remained positive.

Sky ambient is now a deliberately limited, **temporary** illumination term: 0.055 * stellar irradiance, scaled by local twilight/daylight, vertical cloud position, scattering albedo and an attenuation factor exp(-0.20*tauSun). Cloud bottoms and optically thick regions should be darker. It remains an approximation: optical depth toward the Sun is NOT equivalent to hemispherical skylight access. The complete B backend still needs an atmosphere-LUT-driven sky radiance input and/or its own directional ambient optical-depth cache. Do not treat the new constants as calibrated physical cloud illumination.

At typical g=0.72, numerical evaluation of the phase function over the sphere integrates to approximately one after normalization. On an Earth-sized body, the geometric horizon drops by approximately 2.6 degrees at 6.5 km altitude. Planet ellipsoid and offscreen terrain horizon occlusion are intentionally NOT claimed by this spherical B3 approximation.

**Validation:** compare B3 with Sun-cache on and off under noon, low Sun and planet night. Persistent grey on the nightside should disappear. The cloud deck may still appear globally smooth because B2 uses inexpensive sinusoidal density shaping and 64 fixed march steps; that is a separate density and detail representation issue. System A is unchanged.

# Allocation and cache topology

One B-owned SunCache per Studio viewport, stored in unifiedCloudSunCaches_, not in A's cloudLightVolumes_ or cloudTemporal_. Its GPU buffer is 32x32x16 cells, 16 bytes per cell = 262144 bytes (256 KiB). Horizontal tangent-plane sample spacing is 2 km. The volume is centred at a body-fixed unit-direction anchor, not the changing camera float origin, and covers approximately 64 km x 64 km horizontally. Vertically it covers the full current procedural/simulated shell up to the greater of its authored cloud-top altitude and 21 km while a simulated storm is bound.

Cell centres reconstruct a planet-fixed direction from two orthonormal tangent axes, with a pole-safe reference-axis choice, multiplied by planet radius plus altitude. The unit-direction anchor remains fixed as the camera moves locally; it is reset when the camera direction moves more than 16 km of arc from the anchor. This is NOT yet A's toroidal clipmap: no sliding re-use of already completed rows, no second/third long-range cascade. Far clouds outside the local cache fall back to a bounded direct trace.

The buffer is GPU-only and is explicitly zero-cleared on first use via a separate Compute entrypoint. Reads check a float-representable integer generation tag. Invalid or out-of-bounds samples NEVER use stale tau. B's shader samples eight surrounding valid voxels before trilinear interpolation. If any is invalid, the entire sample uses the direct path. This is conservative against blending inconsistent cache generations.

# Update cadence and invalidation

Only ONE altitude slice is written per rendered frame (phase runs 0..15). Each slice contains 32x32=1024 voxels dispatched as 4x4 compute workgroups of 8x8 threads. The nominal full refresh interval is 16 rendered update frames. A new generation becomes progressively ready; while missing samples are detected, the march falls back to a direct ray integration.

Generation invalidation currently covers:
- Weather cloud-field GPU fingerprint change.
- A changed storm-export BUFFER identity (not yet every cell-revision publication).
- Major Sun-direction change (dot < 0.9998 compared with the previous input).
- Body radius change.
- Camera re-anchor beyond 16 km.
- Stable celestial-body ID change (both 64-bit words).
- Simulation time rewind or jump forward greater than 60 seconds (plus nonfinite time).

Continuous Sun motion and storm evolution do NOT reset the cache every frame: the 16-slice time-sliced update gradually refreshes them. This is deliberate to preserve cache utility. Caveat: lighting on recently changed cloud/storm structures may be stale for up to roughly one full cache update cycle. This is a visible latency, not a physically exact cache. Lightning and drastic storm changes need prioritized updates in a later stage.

The explicit 60-second jump guard prevents using cached values from an unrelated state after extreme timewarp, but high timewarp can trigger a full generation reset each frame, causing mostly fallback rays. A future timewarp-aware policy must select lower-detail replay/cached data rather than trying to process unbounded catch-up work.

# Pipeline and ownership

Studio creates the B3 cache lazily and inserts an independent named render-graph pass BEFORE the B view-ray pass:
1. *.UnifiedB.SunCache -- transition buffer ShaderResource -> UnorderedAccess; optionally clear once; time-slice recompute; UAV barrier; transition back to ShaderResource.
2. *.UnifiedB.March -- reads cached tau if valid, else direct light integration; uses the same B scene depth and storm/field inputs.
3. *.UnifiedB.Composite -- retains existing B premultiplied radiance/transmittance blending.
Receiver shadows still run earlier as *.UnifiedB.ReceiverShadow and DO NOT read or multiply A's cloud-shadow target.

Update and read use the same 32-dword (128-byte) shader constants contract. The B medium shader has 3 buffer bindings: B weather, authoritative storm export, B cache. The depth texture is separately bound at slot 3. The compute write entrypoint uses a UAV at buffer binding 2; the read-only fragment variant uses a read-only buffer at the same slot. Shader compiler strips unused entrypoints/resources and the RHI binds the stage-appropriate descriptors.

The existing view Cloud Light Volume toggle controls whether B3 allocates/updates/uses its separate cache. This is a shared USER SETTING, NOT a shared mutable buffer. Turn it off for a controlled B-direct-sun benchmark. Turning it off still uses bounded direct integration; it is not equivalent to B2's old unshadowed approximation.

# Fallback and performance constraints

Cache generation misses, incomplete layers, camera-local volume boundaries and initial warmup use a 6-sample direct Sun integration. Cache voxels are generated with 12-sample direct integration. Both paths clip at the planet and cloud shell. This intentionally limits worst-case extra density evaluations per cloud sample, but does not guarantee that B3 is faster than B2 or A.

The original user's A trace reported ~20.56 ms mean Clouds pass for one scene. THIS IS NOT a B3 result. The B3 cache can be slower while cold, far from the local volume or with high storm density, and it adds a new compute workload every frame. A/B measurement must compare full GPU-frame time, *.UnifiedB.SunCache plus *.UnifiedB.March, and matching images/camera/world/time. Do not claim a speed-up based only on a shorter March pass.

For exact GPU profiling with the existing panel, check:
- *.UnifiedB.SunCache time including initial clear, warmup and steady-state.
- *.UnifiedB.March median and p95.
- *.UnifiedB.ReceiverShadow separately.
- Entire GPU frame median and p95.
- A's existing Clouds/CloudLightVolume timestamps when A is selected.
Capture low Sun, near dense storm, thin layers, orbit and fast camera movement. Compare after 16+ warmup frames and after a forced reanchor/timewarp jump. Avoid claiming B faster if the image lacks comparable shadows/cloud detail.

# Remaining conflicts and next stages

- No cache hit/miss atomic profiling yet. Counters must be sparse/workgroup-aggregated to avoid making the trace itself expensive.
- No independent skylight/ambient cache; no atmosphere transmittance LUT integration; no finite solar-disc penumbra or terrain-relief occlusion in B's cloud direct-scattering path.
- The local 64 km cache is inadequate for planet-wide cloud self-shadowing and long grazing paths. Add physically defined second/third cascade or horizon-domain optical-depth summaries only AFTER measured correctness/perf evidence.
- The cache update is time-sliced GPU COMPUTE in the same render-graph command sequence, not concurrent compute queue scheduling. Asynchronous worker ownership, generation fences and overlap are later milestones.
- The cache's storm-content evolution currently refreshes in 16-frame slices even when the *same* export buffer's voxels change. A published export-generation counter and change prioritization are needed.
- Rendering across multiple bodies/views simultaneously still relies on shared current storm publication in Studio. A per-view pinned storm-export snapshot and lifetime fencing are required for robust concurrency.
- B's receiver-ground-shadow pass still uses an uncached 12-sample ray and does not yet share the same Sun light-volume result; this is an explicit future API connection.
- B2/3 Cloud Lab is intentionally unsupported and suppressed, so the A/B scenario must not use Lab as though B has parity.
- Guard against terminator rings caused by stepping across invalid cache cells or coarse-to-fine transitions. Compare fixed-camera animated Sun captures with cache on/off.
- Cache occupancy, optical-depth reconstruction accuracy and coherence are not GPU-tested yet.

# Orbital B3 ray-length alias diagnosis (2026-10-14)

User evidence: one enabled cloud layer with mean coverage approximately 0.54 and mean optical depth 4.25, GPU field ready. The first visible B3 result showed nearly uniform grey cloud lighting; after normalized scattering and reduced ambient, the updated planet view showed a stronger day/night distinction but also a narrow jagged dark streak around the terminator and repetitive small-scale patterns. These are screenshot observations, not measured GPU attribution.

A source audit identified a separate **sunlight-quadrature bug**: SunDepthDirect previously traced six samples evenly through up to one million metres of the solar ray, even if the ray was mostly outside the actual cloud shell or below its base. Near grazing directions, the distance between samples can greatly exceed cloud thickness. Depending on quadrature phase, the algorithm can report zero extinction or an exaggerated optical depth. That is capable of creating abrupt false bright/dark regions independent of the cache.

The B shader now computes intersections with the inner and outer cloud spheres; it integrates only the occupied segments, distributing its existing bounded sample budget over the two shell intervals if the solar ray dips below cloud base before emerging again. It retains planet solid-occlusion rejection, and B's storm-active flag preserves support for extinction extending down to ground. The local cache uses the same corrected integrator for its slice updates. This does **not** implement fine Sun penumbra, terrain mountain occlusion or long-distance accurate secondary transport, and six samples may still undersample a long grazing cloud path. Tests must include orbital sunrise/sunset and cache enabled/disabled.

The existing viewport now exposes two local controls when B is active:
- B Sun cache ON/OFF: compare cached 12-sample shell optical depth against direct 6-sample fallback, with 16+ warmup frames.
- B receiver shadows ON/OFF: isolate the 12-step surface receiver-shadow pass from the cloud's own in-scattered radiance.

Isolation protocol at identical camera, atmosphere/exposure and simulation time: first toggle receiver shadows OFF; if the jagged dark streak disappears on the planet surface, inspect B receiver shadow depth and compositing. If not, leave receiver shadows OFF and toggle the Sun cache OFF; if that fixes the pattern, inspect cache granularity, generation transition and interpolation. If neither removes it, instrument B's cloud primary step schedule, density and weather/texture sampling before tuning cloud lighting constants. The initial 64-step uniform view march remains a likely source of aliasing at long orbital grazing paths.

The broad pale cloud sheet is a *separate* morphology/LOD issue. B2's density still uses a wide cloud-coverage threshold and a single inexpensive sinusoidal shape modifier. This cannot reproduce fine coherent cauliflower structures and may over-fill most columns at high mean coverage. Do not attempt to solve it by increasing ambient or raising Sun brightness; next stage should analyze coverage-to-opacity statistics and evaluate footprint-aware high-frequency detail with correct far-field optical-depth preservation.

No local Vulkan/DXC build, profiler run or visual regression was completed as part of this fix; these findings remain hypotheses until isolated by the added controls.

# Local checks

1. Run python tools/check_unified_cloud_shaders.py (now compiles vertex, march, ground shadow, Sun-cache compute, Sun-cache clear compute, composite).
2. Build cmake --build build --config Debug --target OrbitStudio OrbitUnifiedCloudFoundationTests.
3. Test ctest --test-dir build -C Debug -R Orbit.UnifiedCloudFoundation --output-on-failure.
4. Run the Studio view toggle 'Cloud renderer: B2 PREVIEW' (label may update to B3) and capture GPU traces with Cloud Light Volume enabled vs disabled on exactly the same camera/scene.
5. Force storm enable/disable, a 60+ second timewarp, reset/teleport, body swap and two docked viewports; verify no NaNs, cloud-light flashes, reused stale planet data or persistent GPU warnings.
6. Compare A before and after selecting B; A's compiled renderer and targets are unchanged.

Read the parent /rendering/clouds/unified-system-b and sibling /rendering/clouds/unified-system-b-density for the source/dependency contracts.
