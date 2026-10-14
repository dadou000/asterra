+++
path = "/rendering/clouds/reference-eve-v5-ledger"
title = "EVE V5 reference implementation ledger and exact Orbit code ownership"
kind = "concept"
status = "proposed"
owner_module = "OrbitCelestialClouds"
summary = "Dependency-ordered implementation blueprint for every cloud/atmosphere/terrain/FX/rendering subsystem, with proposed symbols, existing paths, dependencies and pass/fail contracts."
keywords = ["implementation", "backlog", "Vulkan", "code", "contracts", "integration", "V5"]
related = ["/rendering/clouds/reference-eve-v5"]
depends_on = ["/rendering/clouds/reference-eve-v5-validation"]
+++

# Mandatory engineering operating instructions

Treat this ledger as the implementation **single source of sequencing**, not as permission to invent private shader algorithms. Every item has a measurable API/test output. The new renderer is `EveV5Reference` with its own GPU pipelines, materials, caches and debug namespace; the existing A CloudRenderer and B UnifiedCloudRenderer remain unmodified. Stage work on a dedicated Git branch/PR derived from Harmony and do not activate the new backend by default until F7 visual validation. A/B-only binary must compile without V5 data. Keep Git commits small enough to bisect every subsystem.

**Source categories** used here: O = existing Orbit path verified, P = proposed new code, U = unresolved exact V5 behavior/asset source. Don't write U work as "implemented" until upstream source or golden-run provenance exists.

# Suggested file/module tree (P)

```text
engine/celestial_clouds/reference/
  include/orbit/celestial_clouds/reference/
    ReferenceConfig.hpp           // typed ConfigNode and typed authored params
    ReferenceAssetRegistry.hpp    // legally local-only resource locator
    ReferenceCloudSnapshot.hpp    // immutable fence-owned frame inputs
    ReferenceMedium.hpp           // sigma_t, sigma_s, type, curve/noise
    ReferenceNoise.hpp            // 3D Worley/Perlin/curl generator
    ReferenceGeometry.hpp         // double ray shell, ellipse/depth clipping
    ReferenceLighting.hpp         // Sun/sky direct + multiple approximation
    ReferenceLightVolume.hpp      // optical depth, 12-slice update
    ReferenceShadow.hpp           // receiver Sun transmittance
    ReferenceLod.hpp              // optical-consistent near-to-orbit
    ReferenceTemporal.hpp         // advection, history and disocclusion
    ReferenceRenderer.hpp         // pass graph coordinator
    ReferenceEvents.hpp           // lightning, rain, snow, dust, droplets
    ReferenceDiagnostics.hpp      // GPU stats and failure reasons
  src/
    ReferenceConfigParser.cpp
    ReferenceImporter.cpp
    ReferenceAssetRegistry.cpp
    ReferenceFrameSnapshot.cpp
    ReferenceMedium.cpp
    ReferenceNoise.cpp
    ReferenceGeometry.cpp
    ReferenceLightVolume.cpp
    ReferenceReferenceShadow.cpp
    ReferenceTemporal.cpp
    ReferenceRenderGraph.cpp
  shaders/
    ReferenceShared.hlsli
    ReferenceDensity.hlsli
    ReferenceLight.hlsli
    ReferenceCloudMarch.hlsl
    ReferenceShadow.hlsl
    ReferenceSunCache.hlsl
    ReferenceTemporal.hlsl
    ReferenceComposite.hlsl
    ReferenceParticle.hlsl
  tests/
    ReferenceConfigTests.cpp
    ReferenceGeometryTests.cpp
    ReferenceMediumTests.cpp
    ReferenceLightTests.cpp
    ReferenceLodTests.cpp
    ReferenceTemporalTests.cpp
    ReferenceMultiViewTests.cpp
    ReferenceRhiValidationTests.cpp
engine/studio_ui/src/
  StudioViewportAtmospherePass.cpp   // thin backend dispatch ONLY
  StudioViewportLightingPass.cpp     // reference cloud shadow source
  StudioViewportStorm.cpp            // publish per-view export ticket
apps/editor/src/
  StudioViewportPanel.cpp            // Reference selector/readiness/debug
tools/
  audit_eve_reference.py             // local private ZIP inventory and diagnostics
```

The file tree is an implementation target, not a claim that all these files exist now. Avoid leaving a perpetual temporary monolithic shader or littering Studio with three separate pass graphs. Keep frame-graph coordinator code in the reference module; Studio chooses it.

# Ordered task ledger — EVERY critical subsystem

| ID | New implementation / API | Existing authority / touch point | Depends | Acceptance |
|---|---|---|---|---|
| R00 | Package SHA, license, missing maps manifest | docs/reference-eve-v5-* | none | exact ZIP metadata; no binaries/assets in public repo |
| R01 | ConfigNode parser with repeated values and nested nodes | P/ReferenceConfigParser | R00 | no loss of 293 Hermite key entries |
| R02 | Typed V5 cloud material schema | O/CloudField.hpp for source authority | R01 | 19 layers / 70 types / 27 stamps with optional fields |
| R03 | Hermite curve LUT bake and interpolation | P/ReferenceMedium | R02 | CPU/GPU eval agreement; correct tangent handling |
| R04 | Private asset path resolver, hash and error diagnostics | P/ReferenceAssetRegistry | R01 | all 75 missing logical paths individually reported |
| R05 | Coverage/type/color/flow/SDF texture import and swizzles | O/CloudField.cpp GPU input | R04 | equirectangular/cube-six test and seam continuity |
| R06 | Imported painter tiles/masks and undo/redo | O/Studio Build Procedurals UI | R05 | world-anchored updates, map revisions and trace |
| R07 | Typed layer FX links and time profiles | P/ReferenceEvents | R02 | no unknown implicit settings discarded |
| R08 | Frozen RefFrameKey and revision domains | O/UnifiedCloudFoundation.hpp | R02 | body/world/epoch and dataset hash stable |
| R09 | Lifetime-pinned immutable RefFrameSnapshot | O/StudioViewportRenderer.hpp CloudPresentation | R08 | GPU fence no raw stale pointer on project reload |
| R10 | StormExportTicket per view/body | O/StudioViewportStorm.cpp | R09 | multi-body views never share wrong storm buffer |
| R11 | Body-fixed precision and local GPU origin | O/AtmosphereRenderView | R09 | no cloud drift under floating-origin/teleport |
| R12 | Double shell/planet/ellipsoid ray interval | O/TerrainSource; P/ReferenceGeometry | R11 | two grazing intervals, camera below base, gas giant |
| R13 | Occluder depth interface (reverse-Z) | O/StudioViewportAtmospherePass | R12 | cloud ray clipped at ships/terrain with valid ray distance |
| R14 | Typed cloud density sigma and overlap policy | P/ReferenceMedium | R03,R05,R10 | additive independent media; replacement of simulated convection |
| R15 | 3D tileable noise, spherical billows, erosionDepth | P/ReferenceNoise | R14 | coherent from 100m to orbit; no obvious periodic pattern |
| R16 | Curl warp, flowmap and wind/time advect | O/WeatherModel + P/ReferenceMedium | R14 | cloud motion in body-fixed coordinates, timewarp-safe |
| R17 | SDF and distance-guided skipping | P/ReferenceMedium, ReferenceGeometry | R14 | conservative empty-space skips, no lost thin sheet |
| R18 | Footprint-filtered optical-depth LOD | O/CloudFarField.hpp, optical_transport/CloudRepresentation.hpp | R15 | no coverage/opacity jump at 200–260 km |
| R19 | V5 type-specific layered storm profiles | P/ReferenceMedium | R14,R15 | cumulus/anvil/fog/dust gas-giant charts, exact config data |
| R20 | Source physics to shader ABI versioning | O/rhi/Device and shader Compiler | R14 | ABI reflection/tests and no entrypoint or descriptor mismatch |
| R21 | Direct Sun + atmosphere LUT transmittance | O/AtmosphereRenderer.hpp | R09,R12 | proper scene units and horizon/eclipses |
| R22 | Finite-disc ellipsoid + terrain mountain visibility | O/TerrainSunShadow, AnalyticBodyVisibility | R21 | physically consistent solar penumbra; no double occlusion |
| R23 | B/V5-owned optical-depth light volume | P/ReferenceLightVolume | R14,R20,R21 | 224 horizon resolution/12 slices profile, bounded updates |
| R24 | Sky irradiance/hemispherical cloud openness | O/SkyIrradiance.hpp | R21,R23 | no constant nightside ambient; cloud tops vs bases |
| R25 | Single and multiple scatter phase profiles | P/ReferenceLighting | R23,R24 | energy-normalized and golden-reference calibrated |
| R26 | Camera view-ray integration | P/ReferenceRenderer | R12,R14,R22,R25 | ray completeness, depth, near/orbit continuity |
| R27 | Receiver cloud shadow for direct light | O/StudioViewportLightingPass + DirectLightingRenderer | R12,R14,R22 | red=T_sun; only direct light multiplied |
| R28 | Cloud shadows on atmosphere/godrays | O/AtmosphereRenderer | R23,R25 | cloud shadow enters air scattering source, not final overlay |
| R29 | HDR compositing and aerial perspective | O/StudioViewportAtmospherePass | R26,R28 | scene-linear L and T exactly once |
| R30 | 2D macro-cloud map and scaled transition | O/CompositeOrbitalCloudAppearance; CloudRepresentation | R18,R26 | no double draw and stable opacity in orbit |
| R31 | Temporal cloud motion, jitter, reprojection | O/StudioViewportRenderer CloudTemporal (A only) | R26,R30 | V5-owned history; no thin-layer smear |
| R32 | History invalidation/timewarp/teleport/flash | O/UnifiedCloudFoundation Invalidate | R31,R10 | bounded update; no ghosts or stale world |
| R33 | Precipitation event stream | O/weather_gpu, StudioViewportStorm | R10,R14 | one physical weather source, many visual consumers |
| R34 | Rain/snow/dust GPU particles and splashes | P/ReferenceEvents | R33,R13 | no particles through terrain; independent culling LOD |
| R35 | Lightning optical geometry/flash emission | O/TransientFlashRegistry | R33,R25 | one storm event, localized radiance and cache refresh |
| R36 | Lightning and rain audio source routing | O/sound domains future connector | R33 | source IDs and time/progagation physics consistent |
| R37 | Cockpit glass drops and runoff | O/Material, Surface Buffer, vehicle physics | R33 | motion-correct refraction without global screen smear |
| R38 | Persistent terrain/scenery/craft wetness | O/TerrainSource, material system | R33 | offscreen/timewarp no camera drift |
| R39 | Ocean surface solar/cloud shadow | O/studio lighting ocean/water pipeline | R27,R29 | cloud shadows/direct source agree, one ocean authority |
| R40 | Ocean reflections, caustics, foam, SSR inputs | O/hybrid reflections + ocean modules | R30,R39 | coarse reflected clouds, no second expensive full march |
| R41 | Reflection probes, emissive/lightning clouds | O/HybridReflectionRenderer; radiance cache | R30,R35 | consistent sky HDR radiance, no duplicate exposure |
| R42 | Shadow receiver for ship/building/terrain | O/DirectLighting and proxy shadow | R27 | sun shadow not darken diffuse sky twice |
| R43 | Unified reference quality budgets and LOD | P/ReferenceRenderer | R20,R26,R31 | VRAM/time admission, fallback capabilities transparent |
| R44 | Vulkan graph barriers/fence ownership | O/render_graph RenderGraph; Vulkan RHI | R09,R20,R23 | validation clean, multi-view, resize, project reload |
| R45 | Studio selector and reference Diagnostics | O/StudioViewportPanel, Debug Center | R20,R44 | unavailable/error labelled; no hidden A fallback |
| R46 | Profiling with per-pass GPU timestamps | O/core Profiler, Studio profiler tab | R44 | stage and whole frame p50/p95/p99 and mem |
| R47 | Golden capture pipeline and source fingerprints | O/viewport screenshot + trace sidecar | R45 | exact runtime binary, config hashes, camera/light metadata |
| R48 | CPU/compiled-shader unit tests | P/tests | all subsystems | configs, geometry, source/ABI, NaNs, cache and LOD |
| R49 | 26 synthetic/reference scenes and report | O/Debug Center capture UI | R48 | all M01–M26 pass, no missing private data disguised |
| R50 | Visual validation vs licensed running V5 reference | private legal reference only | R49 | matched config textures camera/time; explicit signed acceptance |
| R51 | Rollout/promote from Preview to Ready | O/Studio UI backend gate | R50,R46 | no A regressions, meaningful GPU budget established |

**No task between R01 and R50 is evidence that the private binaries have been ported exactly.** A task can compile and still be visually nonmatching. Each row gets a follow-up implementation commit, test artifact and profiler trace ID before being marked complete.

# Code-level handoff from Studio

The integration must be a *single* dispatch function, not a branching duplicate of the complete render graph in Studio. Proposed concept:

```cpp
enum class CloudRenderBackend : u8 { BaselineA, ExperimentalB, EveV5Reference };
struct CloudBackendSelection {
    CloudRenderBackend requested;
    bool requireExactAssets;
    u64 userSourceManifestHash;
};
struct CloudRenderOutput {
    rhi::Texture* radianceT;          // integrated scene-linear L plus T
    rhi::Texture* firstCloudDepth;    // optional for temporal/transparency
    rhi::Texture* motion;
    rhi::Texture* receiverSunT;       // red direct solar transmittance
    u64 physicalInputKey;
    bool complete;
};
CloudRenderOutput ComposeCloudBackend(
    CloudRenderBackend selected,
    const std::shared_ptr<const CloudSceneSnapshot>& scene,
    CloudBackendContext& backend,
    render_graph::RenderGraph& graph,
    StudioSceneTargets& targets);
```

These are architectural contracts. To compile, define `StudioSceneTargets`, `CloudBackendContext` and the snapshot under the proper actual Orbit namespaces; do not assume they are already in Harmony. The renderer selection, sky/terrain data, late render-graph command recording and GPU lifetime must all resolve before implementation. Reject B or Reference unavailability explicitly; **never silently switch to A** and report a false comparison.

## Targeted production file changes (exact existing path)

- `engine/celestial_clouds/CMakeLists.txt`: optional independent Reference target and unit tests, no links against private DLL/Unity assets.
- `engine/studio_ui/include/orbit/studio_ui/StudioViewportRenderer.hpp`: one backend selector and a single per-view backend-state handle, replace global sharedStormCloudExport_ in new backend with fence-pinned ticket. Existing A/B state maps retained until full migration.
- `engine/studio_ui/src/StudioViewportAtmospherePass.cpp`: only dispatch the selected renderer's view/resolve/composite, use atmosphere from shared authoritative view and one HDR output.
- `engine/studio_ui/src/StudioViewportLightingPass.cpp`: select the correct renderer's receiverSunT into existing direct lighting; any cloud shadow double-application is a failure.
- `engine/studio_ui/src/StudioViewportStorm.cpp`: publish immutable per-body/per-view source generation; no extra simulation step for Reference backend.
- `engine/celestial_clouds/src/CloudField.cpp`: if additional authored map support needed, add separately versioned read-only adapter; do not reorder existing 16-byte GPU weather texel or B descriptor layout.
- `engine/celestial_atmosphere/src/AtmosphereRenderer.cpp`: optional reference cloud Sun visibility factor and phase/coherent air integration; do not fork global physical sky model.
- `engine/lighting/src/DirectLighting.cpp`: not a new cloud shader, only optional source selection if existing cloudShadow input can remain stable.
- `engine/lighting/src/TerrainSunShadow.cpp`: share terrain visibility as source, never create conflicting renderer-specific terrain raymarch authority.
- `engine/terrain/include/orbit/terrain/TerrainSource.hpp`: no change to authority or coordinate meaning. Cloud reference must query thread-safe surface sampling, not mutate it.
- `apps/editor/src/StudioViewportPanel.cpp`: tri-backend selector, source/asset readiness, reference quality profile, diagnostic visualizations.
- Existing profiler and Debug Center modules: classify all new `.EveV5Ref.*` GPU passes, allow capture/report, include source hash; never add a second profiler implementation.

## Progress taxonomy and acceptance

- `SOURCE-VERIFIED`: matches user's licensed original V5 C#/HLSL code and shader build, or a controlled empirical reference with tolerance.
- `CONTRACT-COMPLETE`: typed API, test and diagnostics specified, code may be unimplemented.
- `IMPLEMENTED-UNVERIFIED`: branch builds but no GPU/render validation.
- `SMOKE-PASS`: Vulkan launches on synthetic fixtures; not visual parity.
- `VISUAL-PARITY`: golden scenes under legal reference assets within error budget.
- `RELEASE-READY`: resource lifetime, timewarp/multiview, performance, A rollback and legal provenance pass.

At writing, this documentation set can only be **CONTRACT-COMPLETE for the design**, not SOURCE-VERIFIED for missing proprietary internals and not IMPLEMENTED for the new renderer. A/B work from prior turns has runtime user captures; it does not grant EVE reference readiness.

# Items that NEED explicit upstream information

1. V5 GPU shader sources and plugin source corresponding exactly to ZIP's build (not 2019 public EVE code).
2. Legal reuse/port and redistribution permission for those sources, bundle-derived assets and accompanying textures.
3. Missing original coverage/type/flow/SDF maps; known map channel/swizzles and cube-face orientation.
4. Ground truth scene captures at matched Sun/camera/body, shader settings, capture methodology and colour output.
5. Unity-KSP material pipeline, depth ordering and temporal motion vector details beyond the shipped release notes.
6. Absolute stellar radiance/tone-map exposure calibration for Scatterer artistic multipliers.

The project can execute synthetic prototype work without those, but **cannot truthfully deliver “exactly as it is in the file”** until they are provided/verified. Always label the difference.
