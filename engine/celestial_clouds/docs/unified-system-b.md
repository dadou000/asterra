+++
path = "/rendering/clouds/unified-system-b"
title = "Unified Cloud System B: isolated backend architecture"
kind = "concept"
status = "experimental"
owner_module = "OrbitCelestialClouds"
summary = "Foundations and non-negotiable integration rules for a separately owned, physically coherent planet-scale volumetric cloud backend B, benchmarked against unchanged CloudRenderer A."
keywords = ["System B", "cloud renderer", "independent", "A/B", "unified cloud", "weather", "lighting", "timewarp", "light volume", "temporal", "cloud fallback", "render graph"]
sources = [
  "engine/celestial_clouds/include/orbit/celestial_clouds/UnifiedCloudFoundation.hpp",
  "engine/celestial_clouds/tests/UnifiedCloudFoundationTests.cpp",
  "engine/celestial_clouds/include/orbit/celestial_clouds/CloudRenderer.hpp",
  "engine/celestial_clouds/include/orbit/celestial_clouds/CloudField.hpp",
  "engine/studio_ui/src/StudioViewportAtmospherePass.cpp",
  "engine/studio_ui/src/StudioViewportLightingPass.cpp",
  "engine/studio_ui/src/StudioViewportStorm.cpp",
  "engine/lighting/include/orbit/lighting/SunVisibilityShader.hpp",
  "engine/celestial_atmosphere/include/orbit/celestial_atmosphere/AtmosphereRenderer.hpp",
  "engine/studio_ui/include/orbit/studio_ui/StudioTerrainLayerOptions.hpp",
  "engine/studio_ui/src/StudioRenderViewRpc.cpp",
  "tools/orbit_ab.py",
  "tests/CloudLightVolumeTests.cpp"
]
symbols = [
  "orbit::celestial_clouds::unified::CloudBackend",
  "orbit::celestial_clouds::unified::FrameSignature",
  "orbit::celestial_clouds::unified::Invalidate",
  "orbit::celestial_clouds::unified::ViewRegistry",
  "orbit::celestial_clouds::unified::ResourceBudget",
  "orbit::celestial_clouds::unified::IntegratedMedium",
  "orbit::celestial_clouds::unified::CompositeFrontToBack"
]
related = [
  "/rendering/clouds",
  "/rendering/clouds/raymarch",
  "/rendering/clouds/shadows-and-light-volume",
  "/rendering/clouds/references/blackrack-eve-orbit-design",
  "/rendering/lighting/terrain-sun-visibility"
]
depends_on = ["/rendering/clouds", "/rendering/atmosphere", "/rendering/lighting"]
[routes]
"where system A and system B are separated and why" = "/rendering/clouds/unified-system-b"
"what needs wiring before B can be rendered and benchmarked" = "/rendering/clouds/unified-system-b"
"how B will interact with terrain, atmosphere, reflections, storm physics and audio" = "/rendering/clouds/unified-system-b"
+++

# Purpose and phase status

Build a production-worthy **Unified Cloud System B** without modifying the rendering algorithms of **existing System A**. System A is `celestial_clouds::CloudRenderer` and all existing GPU passes/caches in Studio. It remains the default, the golden reference and the rollback path. The old `clouds.split_set` right-hand variant is **A with alternate defines**, NOT the new B. Never label it A/B-backend comparison.

Current milestone **B1 experimental preview**: B0 CPU contracts remain in `UnifiedCloudFoundation.hpp` with CTest coverage, and `UnifiedCloudRenderer.hpp/.cpp` now provide B-owned Vulkan shader pipelines for a bounded analytic weather-cloud march, receiver shadow and alpha composite. Studio can select `cloud_b_preview` per viewport through `view.terrain_layers_set`, or the visible viewport control. B targets and timing pass names are independent (`.UnifiedB.*`). This is not the production-quality or fair-performance A/B mode: no B light-volume cache, storm-grid extinction import, cloud-lab override, temporal resolve, point-light scatter, true aerial perspective, or multi-layer integration yet. The preview has separate B receiver shadows, but these are approximate. B compilation is lazy and nonfatal to A; failure is reported in the viewport. A remains unchanged/default. The full backend gate remains closed until the independent capabilities exist.

This is not a proposal to build a second weather or storm physics simulator. Those represent the planet and are authoritative shared *inputs*. Both rendering backends must see the same simulation result at the same absolute physical time. They must **not** share caches of rendered radiance or temporally accumulated cloud images.

# Explicit ownership graph

```text
                    AUTHORITATIVE NON-RENDER SYSTEMS
   Universe/BodyRegistry + FrameGraph + absolute SimulationTime
   WeatherModel/CloudField + zonal storms + GPU storm solver export
   TerrainSource/TerrainSunShadow + terrain revisions/planet shape
   CelestialLighting/Helion + AtmosphereParameters + atmosphere LUTs
   Local light authoring + lightning and storm electrification
                              |
                   immutable frame snapshot
                    /                   \
          backend A (existing)     backend B (new)
          CloudRenderer            UnifiedCloudRenderer [future]
          ACloudLightVolume        BLightingCache [future]
          ACloudShadow             BCloudShadow [future]
          ACloudTemporal           BCloudHistory [future]
          ACloudTarget             BCloudTarget [future]
                    \                   /
                 COMMON CONSUMER CONTRACTS
           cloud RGBA: scene-linear in-scatter + T
           direct sunlight receiver transmittance
           atmosphere/planet visibility and reflection inputs
           depth, multi-view, viewport composite and profiler
```

A and B MUST NOT share output/history targets, sun depth light caches, intermediate density reconstruction, update generation, GPU shader pipelines or mutable renderer state. Their common data are inputs and stable underlying physical definitions only. Their shaders must not call each other's implementation functions. A compute pass for B must use a `.CloudB.*` namespace and independent resource ownership. A failure/disable of B must leave the A world unchanged. Distinct frame timing and memory reporting is mandatory.

The physical medium itself is conceptually unified inside B across cloud subtypes and local storm sources. It does not mean duplicating or fusing all of Orbit's weather/volumetric/audio systems into one expensive universal GPU solver.

# B0 implementation: what is already implemented

`UnifiedCloudFoundation.hpp` provides:

- `CloudBackend`: explicit A or B identity. `RequireAvailableBackend(UnifiedB,false)` throws instead of silently using A or depicting no clouds as a valid B benchmark.
- `FrameSignature`: stable world generation, body ID/key, terrain revision, cloud-field fingerprint, storm revision, atmosphere revision, Sun revision, quality revision, viewport size and discontinuity flag. Future host wiring must compute these from authoritative inputs, not hash transient pointers.
- `Invalidate(previous,next)`: distinct density, sunlight, aerial perspective, temporal history, targets and receiver-shadow invalidation. Tests verify a continuous Sun change does not reset density and every temporal frame; ordinary weather evolution can reproject its history, whereas a discontinuity, world reload or body replacement invalidates everything.
- `ViewRegistry`: stores independent signatures by viewport ID, with `Forget` on view destruction and `Reset` on project/world teardown. Do not key B state as `studio.primary.B` because A already uses that suffix for its shader-define split. Name GPU targets as `<view>.UnifiedB.*` when implemented.
- `ResourceBudget`: per-System-B default cap 256 MiB, with per-frame update and ray step limits. This is a CONTRACT, not a working allocator/eviction implementation; runtime enforcement remains to do.
- `IntegratedMedium` and `CompositeFrontToBack`: scene-linear, already-integrated radiance + scalar transmittance. Front-to-back combine is `Lnear + Tnear*Lfar`, `Tnear*Tfar`. Tests require validity, identity and associativity. This prevents duplicate cloud layers and incorrect extinction blending.
- `BackendReadiness`: B cannot be considered a valid A/B render result until its own march, sun cache, history, compositor and receiver shadow path are present.

`Orbit.UnifiedCloudFoundation` tests are registered under `engine/celestial_clouds/CMakeLists.txt`. The new sources have NOT been built or run on the user's Windows Vulkan hardware yet.

# Cross-system integration audit (actual Harmony code)

| Upstream/downstream | Existing A integration | Mandatory B contract / preventive fix |
|---|---|---|
| Universe / body frames | `StudioViewportAtmospherePass.cpp` reads body, shape, body-space camera and Sun | Use physical body-fixed double world position, time and unique stable body identity; never cache by float camera-origin or swap a cache between planets. Apply real ellipsoid axes for eclipses even if cloud shell uses a reference radius. |
| Cloud weather field | `CloudFieldProduct`, `GpuCloudFieldProduct` have face resolution, fingerprint, multiple layers; A near-field reads the first | Shared immutable cloud-field product is valid; B must consume all relevant layers and their semantic IDs, but not rebuild weather for each viewport or inherit A's single-first-layer constraint. |
| GPU storm solver | `StudioViewportStorm.cpp` exports placed/nested storm extinction, interpolation sponge and evolving simulation time; A consumes `SetStormBuffer` | B consumes the same authoritative storm export with an adapter and a GPU generation fence. Do not run the storm physics twice. Correctly blend domain edges; force temporal/light-cache invalidation on checkpoint swap or time discontinuity. |
| Celestial atmosphere | `AtmosphereRenderView` has body-fixed camera, Sun, irradiance scale, solar angular radius, ellipsoid axes, terrain visibility and LUTs | Share these immutable inputs, not a second independent atmospheric extinction model. Source solar irradiance must have one unit definition. Never multiply planetary eclipse or terrain shadow twice. |
| Surface direct lighting | `StudioViewportLightingPass.cpp` sends `CloudShadow` to shared direct lighting, multiplying the stellar term | B must generate its own receiver transmittance and switch the *input* to common direct lighting. Shadow must only affect direct illumination, not incorrectly darken diffuse sky fill; don't run A and B shadow multiplication together. |
| Terrain mountain occlusion | `TerrainSunShadowField` body-fixed heightfield and finite-disc visibility, referenced in cloud and atmospheric lighting | B samples the same terrain visibility authority. If B later gets a hierarchy or shadow contours, expose them through a versioned shared query API, not conflicting visibility results. Invalidated terrain cannot leave stale B lighting. |
| Cloud light volume | A owns 3 camera-centred toroidal cascades; settled-input fingerprint and GPU tags, optional in-march fallback | B owns a separately versioned cache, physical optical-depth integration interval and confidence flags. A cache pointers and generation IDs are prohibited; cache refresh cannot spuriously invalidate on every camera movement. |
| Atmospheric composite | A atmosphere pass occurs before clouds; A ray marcher includes its own camera-to-cloud aerial perspective | B must neither composite air scattering twice nor treat the already atmosphere-composited scene as unattenuated. First B milestone can reproduce the same physical boundary contract, then develop unified segment integration independently. |
| Cloud shadow on air / god rays | A's separate shadow/light path affects atmospheric in-scatter | B must expose one consistent solar transmittance field for air and cloud, with atmosphere/terrain occlusion separated from cloud optical depth; avoid post-process darkening. |
| Near/far clouds | Orbit shares weather field with orbital macro-globe, avoids drawing the near-field march and macro globe simultaneously | B cannot run a second cloud overlay over the macro globe; implement a representation handoff keyed to apparent footprint, orbit altitude and same optical-depth statistics. |
| Water/glass/mirrors | Many downstream passes sample scene radiance and reflections; cloud image exists in HDR composite | Reflections/probes should receive a consistent lower-cost B cloud radiance/opacity representation, with stable exposure. Do not spawn a full second cloud raymarch per reflected pixel by default. |
| Volumetric fog/aerosols | Different density representations and LUTs exist in optical transport/volume authoring | Adopt an extensible typed extinction+source contract. Do not force all fog, fluid and weather phenomena into the same dense cloud voxel resource. Prefer world-space segmented integration with mixed media only where physically overlapping. |
| Lightning/local lights | `GatherCloudPointLights` supplies transient flashes and authored light set; `WatchStrikes` and arc overlays are downstream of cloud composite | B consumes the *same lightning events* and radiometric point-light descriptors, not an independent bolt simulation. Flashes require high-priority lighting refresh/history rejection and correct event-driven sound-domain mapping. |
| Sound domains | Sound spatial propagation is separate from visual cloud transport | Lightning event identity, position and exact physical time must be shared. Render-dependent optical caches have no authority over thunder propagation. |
| Runtime editor / project | Viewports can resize, split, hot reload, rebuild and change bodies mid-flight | Per-view B histories and allocations must be cleaned up on view deletion, project reload and device reset; shader compile errors retain last valid B generation, and cannot destroy A state. |
| Timewarp | Weather, storm and simulation clocks may skip or replay stages | Distinguish smooth Sun/advection from discontinuous timewarp; discard reprojected images at epoch jumps and reject stale asynchronous cloud/light jobs with generation checks. |
| Vulkan RHI / profiler | Render graph requires explicit texture state/access and pass dependency; GPU pass timers are available | Separate `.UnifiedB.March`, `.UnifiedB.LightCacheUpdate`, `.UnifiedB.Shadow`, `.UnifiedB.Resolve`, `.UnifiedB.Composite`. Ensure barriers and fence-pinned input lifetimes; do not pretend arbitrary async compute is safe. |

# Frame-time, rendering-quality and reference isolation

A/B comparison will eventually have these modes:
- **A-only**: exactly existing cloud shader/passes, no B allocations or execution.
- **B-only**: only B cloud shade/light/cache/history/receiver-shadow passes; stable shared atmosphere, terrain, weather, renderer UI, lights and simulation are unchanged.
- **A-vs-B split**: both backends draw the same physical scene/time and independent light/caches/histories, compositing to left/right of the same output. The split is a visual diagnostic and its whole-frame GPU time is NOT an A or B performance measurement, because BOTH renderers execute.
- **A/B sequential profiler**: same output resolution, camera, weather and time, pinned exposure, identical warmup and A interleaved repeat; capture whole GPU frame, per-pass GPU timestamps, stage budgets and quality-image reference.

B allocation policy: on B-only, free retired A per-view cloud scratch when GPU fences allow while retaining immutable shared cloud/weather resources; on A-only free retired B scratch. During split, account for simultaneous memory. Do not run both complete pipelines invisibly during single-mode measurements.

The existing A shader-variant suffix `.B` does not denote the new backend. B1 now uses `.UnifiedB.CloudMedium`, `.UnifiedB.March`, `.UnifiedB.Composite`, `.UnifiedB.CloudShadowTarget` and `.UnifiedB.ReceiverShadow`. `clouds.split_get/set` still compares two A shader variants; its overlay is hidden in B preview. The `cloud_b_preview` selector is explicitly labelled experimental. It permits debugging first pixels and Vulkan integration, **not** claiming full backend parity. Later expose true A-only, B-only and A-vs-B visual comparison only after `BackendReadiness::FullABReady()` and scene equivalence tests.

When rendering the same scene in A and B, share **depth, physical timestamp, exposure and atmosphere/terrain data**, but do NOT share a temporally accumulated radiance history. Disabled A clouds must not leave a stale A cloud shadow multiplied into B's sunlight.

# Physical consistency and no-double-count contracts

1. **Scene units**. Internally store cloud extinction sigma_t and scattering sigma_s in m^-1, path length in metres, optical depth dimensionless, solar irradiance/radiance in Orbit scene-linear reference units. Keep scalar optical depth initially if absorption is spectrally neutral; explicitly version the schema before coloured extinction is introduced.
2. **Visibility factors**. Separate `T_cloud = exp(-tau_cloud)`, atmospheric extinction `T_air`, finite solar disc/planet occultation `V_planet`, local terrain horizon `V_terrain`, and proxy/mesh shadow `V_solid`. Compose as a product for direct source visibility only when each factor represents a distinct non-overlapping occlusion mechanism. Store opacity and radiance separately; don't multiply cloud attenuation into both LUT and final lighting.
3. **View-ray integration**. For near segment N followed by far segment F, `L = L_N + T_N*L_F`, `T=T_N*T_F`. The B CPU contract tests this. At each sampled position, medium extinction of physically overlapping types sums before calculating total T. Mixing precomposited layers in an arbitrary object order cannot reproduce this.
4. **Sun-direction cache**. Define the exact ray interval and any near/far partition for each cached `tau_sun` value. A local high-detail correction must not integrate the same Sun segment a second time or cause annular light transitions at cascade boundaries.
5. **Input revision**. Differentiate content changes from smooth motion. Ordinary Sun rotation refreshes lighting but need not invalidate temporal history every frame; hard jump, world replacement and invalid/provenance cache all clear their dependent states.
6. **Storm state**. Storm simulation and procedural background cloud blending must share a single physical extinction field; the visual noise only adds valid, bounded detail. When a storm becomes playback, keep physical-time provenance and smooth transition without stale lighting.
7. **Point lights**. Lightning and emissive structures are not additional constant directional Suns. Use local inverse-square/attenuation and avoid treating the GPU brightness threshold as emitted radiometric flux.

# Conflict register / decisions to make BEFORE implementing GPU B

**C-01: B vs A ownership.** Never re-use A `CloudRenderer::LightVolume`, persistent `cloudTargets_`, `cloudTemporal_`, `cloudShadowTargets_`, its shader defines or its `studio.primary.B` suffix. New B module + per-view target maps. Required A identity/regression capture on every integration change.

**C-02: Shared GPU field lifetime.** Current Studio `cloudPresentations_` stores `std::unique_ptr<GpuCloudFieldProduct>` and `std::unique_ptr<CloudFieldProduct>`; graph lambdas use raw pointers while frame/generation lifetimes are controlled elsewhere. B should hold a render-generation-pinned shared snapshot of the exact field, atmosphere LUTs, terrain visibility and storm export, or inherit an explicit render-graph lifetime guarantee. Do not blindly cache those raw pointers into asynchronous B jobs. A's lifetime may remain unchanged unless separately justified.

**C-03: Camera and planet origin.** Body-relative double position on CPU, camera-relative float in GPU input. At >100 km, absolute float world position loses usable cloud/terrain detail. GPU shader origin/axes must be consistent with atmosphere and storm exporter. Never key B cache on a current floating-origin offset.

**C-04: Epoch discontinuity.** In the current code, weather fingerprints/time bucket can change continuously; invalidating *all* temporal history at every time bucket would destroy upscaling. B's `FrameSignature` invalidates density/light caches while allowing smooth reprojection on such changes, with an explicit discontinuity flag for leaps. Full physics timewarp contracts need host wiring.

**C-05: Planet eclipse vs mountain horizon.** The shared terrain visibility is a spherical heightfield while `AtmosphereRenderView` carries true ellipsoid axes for planet eclipse. Do not let B restore spherical eclipse where the active renderer uses an ellipsoid. Full direct solar visibility, cloud relighting and ground shadows must be compared at dusk and offscreen mountain horizons.

**C-06: Numerical conditioning.** Optical depth calculations use stable near-zero extinction limits. At large depth, allow `exp(-tau)` to approach zero; no NaN propagation, negative scattering, or white discontinuities. For extreme Sun grazing rays, perform conservative intersection with curved atmosphere/cloud shells and avoid horizon LUT interpolation of fully blocked ground rays.

**C-07: Cache invalidation and asynchronous writes.** Every async product must carry body, input fingerprint, source revision and generation; publish only after matching active view. A late worker result after world reload must be discarded. GPU resources may retire only after frame fences.

**C-08: Multi-view.** Multiple docked views, reflections and captures cannot steal the same camera-relative light cache and re-anchor it repeatedly; B state keyed by stable viewport + body plus field generation. `ViewRegistry` is the CPU part; GPU lifetime management remains future work.

**C-09: Adaptive update budgets.** Use B `ResourceBudget` as a policy, not a claim about current allocation enforcement. Global weather/storm solver and local B render caches must share an overall VRAM envelope but have different tick rates. Never let timewarp trigger unbounded catch-up dispatches.

**C-10: Reflection and HDR.** Share physical radiance outputs, not tone-mapped screenshot colour. Atmosphere/cloud colour transformations should be performed exactly once in the common post-processing pipeline. Reflection probes and smooth glass need stable scene-linear fallback.

**C-11: A/B integrity.** A compile error, missing cache or unsupported GPU capability must produce an explicit B status/failure; no silent switch to A for B measurements. In split, A and B sample exactly the same world snapshot and exposure; no physical simulation double-step.

**C-12: Future universal volumetric media.** Do not prematurely merge ocean water, structural smoke, humidity, sound domains and clouds into one 3D field. Use common extinction/radiance/light/event contracts, but source-appropriate sparse solvers. Mixed media integration is a renderer-level operation.

# Phased construction sequence

**B0 Foundation [implemented, not runtime validated]:** independent backend enum, explicit readiness gate, per-view invalidation/domain registry, radiance/transmittance compositing, memory/update policy and CPU tests. Entire A render graph is unchanged.

**B1 Working B preview [code committed; runtime unverified]:** independent `UnifiedCloudRenderer` compiles separate 64-step cloud march, 12-step receiver shadow and premultiplied alpha compositor, reading the original immutable cubemap weather buffer, using distinct shaders/targets and `.UnifiedB.*` pass names. `StudioViewportLightingPass` selects a B-owned receiver shadow, and `StudioViewportAtmospherePass` switches the cloud march/composite; shared storm update and lightning-arc drawing remain outside the A/B branch. `cloud_b_preview` defaults off and is exposed via RPC and the viewport. On B compilation failure A still boots, while the selected preview reports the error rather than quietly drawing A. Admission checks cap each B preview target to half the currently declared 256 MiB budget; aggregate cache/buffer enforcement remains pending. The shader has not been compiled by DXC or run on the GPU in this engineering session; do not claim a successful rendered comparison. Open work: correct scene-equivalent storm/lab density, atmospheric and finite-disc sunlight, long-range light cache, history, overlapping layers, and actual measured GPU performance.

**B2 Unified density representation:** field adapter for global weather, procedural cloud types and simulated storms; typed extinction/phase outputs; local sparse density; high-frequency shape only near visible surfaces; accurate world/unit frames. Acceptance: distant clouds, local storm and orbital representation agree on position and approximate optical thickness.

**B3 Sun/ambient lighting cache:** B-owned sparse multiscale optical-depth representation, independent update cadence and validity tags. Use a cheap per-pixel lookup with bounded local skin correction, preserving transmittance and physical integration intervals. Feed cloud-on-cloud, receiver ground shadows, atmospheric god rays and direct illumination consistently. Separate skylight from direct Sun.

**B4 Temporal/reconstruction:** B-owned temporal history, depth-layer metadata and cloud advection motion; checkerboard/partial-rate sampling with robust disocclusion, lightning and timewarp handling. Compare A and B images after convergence in rapid flight and sunset scenes.

**B5 Atmosphere and reflection integration:** per-segment mixed-air/cloud scattering, terminator/ellipsoid/terrain occlusion, orbital-to-ground handoff, reflection/probe budgets and local emissive lighting. End-to-end tests verify no double-counted medium or shadow source.

**B6 Optimization and A/B council:** conservative empty-space masks/SDF only where profiling demonstrates gain; hierarchical stepping, multiple material LODs, adaptive cache refresh, low-end VRAM ceilings. Run repeated GPU profiler captures across orbit, ground, storm interior, low Sun, multi-view and timewarp. Accept B only if actual GPU-frame improvement and visual integrity pass, with A retained indefinitely.

# Test suite and next verification

The new CTest `Orbit.UnifiedCloudFoundation` tests:
- nonready B backend explicitly fails without touching A;
- independent view registry, forgetting views and resetting;
- separately scoped camera resize, Sun rotation, weather change, terrain edit, atmosphere edit, world replacement and time discontinuity;
- per-B resource policy validity;
- front-to-back radiance accumulation, identity, associativity and invalid-input rejection.

Local host validation still required:
```
cmake --build build --target OrbitUnifiedCloudFoundationTests --config Debug
ctest --test-dir build -C Debug -R Orbit.UnifiedCloudFoundation --output-on-failure
```
The test is hardware-independent and requires no Vulkan device, but it was not run remotely at documentation-commit time. Later B milestones require DXC/Vulkan shader build and real GPU testing.

B1 includes a separate shader compilation harness:
```
python tools/check_unified_cloud_shaders.py
```
It extracts the actual embedded B vertex/march/receiver-shadow/compositor source and compiles each entrypoint with glslangValidator for Vulkan 1.3 (requires the Vulkan SDK or glslang-tools). Shader compilation is NOT a GPU visual validation. The first B preview selection compiles lazily via Orbit's normal shader::Compiler; if it fails, the viewport displays the error and A remains launchable. No hosted Windows build or physical Vulkan run was available in this commit sequence.

To switch on the experimental preview with MCP/RPC: `view.terrain_layers_set` using `{"id":"studio.primary","values":{"cloud_b_preview":true}}`, or use the viewport's `Cloud renderer: B1 PREVIEW` button. The reverse choice `false` restores A. Benchmark pass names are `studio.primary.UnifiedB.March`, `studio.primary.UnifiedB.ReceiverShadow` and `studio.primary.UnifiedB.Composite`; these are diagnostic timings only until the backends have equivalent features and identical workload. Capture both radiance differences and CPU/GPU timings once B reaches full readiness.

Use the repository's existing Debug Center and Profiler for all B test modes; do not add a second standalone launcher or screenshot pipeline.

# Research grounding

- /rendering/clouds/references/blackrack-eve-release5: evidence-ranked inspection of the user-supplied EVE Release 5 distribution (binary-only) and its lighting/temporal/cache approach.
- /rendering/clouds/references/blackrack-eve-orbit-design: detailed Orbit-specific A/B shader-ablation plan and implementation structure.
- /rendering/clouds/references/library: Nubis³, Unreal, Hillaire, Vulkan code and EVE docs.

Do not vendor proprietary KSP/EVE/Scatterer binaries, textures or code.
