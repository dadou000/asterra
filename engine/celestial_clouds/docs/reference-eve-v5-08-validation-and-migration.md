+++
path = "/rendering/clouds/reference-eve-v5-validation"
title = "EVE V5 reference validation, reproducibility and phased migration"
kind = "concept"
status = "proposed"
owner_module = "OrbitCelestialClouds"
summary = "Executable scenario matrix, golden reference capture policy, optical accuracy metrics, performance admission, rollout phases and no-false-parity gates."
keywords = ["EVE V5", "validation", "golden images", "performance", "capture", "integration", "tests"]
related = ["/rendering/clouds/reference-eve-v5"]
depends_on = ["/rendering/clouds/reference-eve-v5-render-graph"]
+++

# Completion criteria, in strict order

The user asked for the supplied EVE/Scatterer renderer **as a stable known-working reference**. Neither the screenshots of existing Orbit A/B nor this binary ZIP establish an exact Vulkan port. Acceptance requires:
1. Legal source and complete texture sets, OR a separately measured behavioral reproduction clearly identified as such. The user-provided ZIP's copyright/license prevents treating bundled shaders/DLLs as redistributable source.
2. The same scene, weather/cloud map, Sun/atmosphere, time and object positions on the original KSP reference and Orbit. Missing maps cannot be silently replaced; synthetic textures are valid only for shader/geometry smoke tests.
3. A correctly compiled `EVE_V5_REF` Vulkan backend, with independent GPU allocations/shaders, never calling A/B renderer code except shared physical input and common scene output API.
4. Independent checks for near-ground, cloud interior, high altitude, orbit, storm, nighttime, eclipse, precipitation, reflected view and timewarp. Cloud-region shape and visibility must survive representation/LOD changes.
5. Controlled optically meaningful image comparison at locked exposure, with whole-frame GPU profiler cost measured and budgets enforced. Any missing feature is an explicit FAIL / UNSUPPORTED, not green checkmark.

# Deterministic golden-scene matrix

Capture **at least these 26 scenarios**; each fixture has a reproducible body/time/camera/Sun/cloud profile seed/asset manifest plus expected optical properties. Build/import multiple realistic clouds only after private reference assets are legally available.

| ID | Scenario | Required assertions |
|---|---|---|
| M01 | Empty atmosphere, no cloud layer | cloud T=1, L=0, no GPU pass crash |
| M02 | Single stratus over flat terrain | base/top altitude, no repeating square tile |
| M03 | Isolated cumulus | 3D silhouette, interior depth, correct motion |
| M04 | Mixed stratus/cumulus | type map interpolation/curve fidelity |
| M05 | Dense cumulonimbus 11 km | tower, anvil, optical-depth response |
| M06 | Thin cirrus above convection | overlapping-layer transparency, no smear |
| M07 | Fog near surface | scene-depth clipping, silhouettes through fog |
| M08 | Rain shafts | precipitation and wetness same storm input |
| M09 | Snow particles / FX-only | visuals present with zero fake cloud volume |
| M10 | Duna dust storm | correct colour/density/flow, low sky visibility |
| M11 | Jool negative-altitude layers | no hard solid-world clip, nested clouds |
| M12 | Eve overlapping layers | ordered blending, independent phase parameters |
| M13 | Laythe pyrocumulus | vertical placement and flow continuity |
| M14 | Cloud 100 m away | microdetail with no regular hash/grid repetition |
| M15 | 2 km in cloud layer | near 3D detail, volumetric density/tau |
| M16 | 10 km above deck | convincing horizon/side walls, cloud tops |
| M17 | 100 km altitude | curved shell interval, world position stable |
| M18 | 200–260 km transition | no double overlay and no opacity discontinuity |
| M19 | Planetary limb from orbit | no square patches; clouds remain visible |
| M20 | Morning Sun at 5° and 0° | finite solar disc, low-angle shadow stability |
| M21 | Night, moon/eclipse | night clouds dark absent local emissions |
| M22 | Mountain blocks above-horizon Sun | terrain visibility authoritative |
| M23 | Ocean reflection and waves | reflected cloud LOD stable; one ocean owner |
| M24 | Cockpit glass in rain | droplets/streaks + real glass motion |
| M25 | Supercell lightning, multiple strikes | optical flash, thunder event, time/replay |
| M26 | Extreme timewarp and teleport | bounded catch-up, no stale history/cache |

Also run orthographic/top-down orbit, grazing below cloud base with two disjoint shell intervals, high-speed camera motion, 2-viewport multi-body, post-hot-reload, 16K screenshot, no ray-tracing GPU, shader compile failure and deliberate missing textures. Those are cross-cutting contract tests, not substitutes for M01–M26.

# Capture format and provenance

Use the existing Orbit screenshot/capture/profiler infrastructure. Every capture must have:
- Renderer identity A/B/EVE_V5_REF and exact executable build fingerprint (the running binary, not latest Git HEAD); commit SHA/dirty state and shader hash.
- Reference artifact SHA, release config fingerprint, texture/sound asset manifest hash, synthetic fallback flag.
- Body ID, cloud layer type schema revision, weather field revision, storm export generation, terrain and atmosphere LUT revisions.
- Physical epoch, camera body-fixed double position and orientation, projection, near/far, viewport resolution, render scale, jitter seed, temporal frames warmed.
- Star direction, angular radius, irradiance, exposure/eye-adaptation state, output colourspace, temporal/SSR/deferred settings, GPU vendor/device and driver.
- Cloud pass GPU times, whole-frame GPU times and memory residency, calibration scene name and quality preset.

**EXISTING-ORBIT** has Profiler GPU pass trace exporting Chrome/Perfetto JSON. Do not make screenshots in lieu of requested GPU profiler traces. Baseline context: an earlier A trace measured ~20.56 ms mean clouds pass in one particular scene. That number is NOT a reference performance target for V5 on other settings.

# C++ independent numerical references

Add public synthetic-only unit tests (no proprietary release payload) with separately authored known solutions.

```cpp
void BeerLambertFixture()
{
    constexpr double sigmaT = 0.002;
    constexpr double segmentMeters = 800.0;
    const double expected = std::exp(-sigmaT*segmentMeters);
    const double a = IntegrateConstantMedium(sigmaT,segmentMeters,128);
    assert(std::isfinite(a) && std::abs(a-expected)<1.0e-5);
}
void TransmittanceOverlapFixture()
{
    const double length=250.;
    const double a=0.0015,b=0.003;
    const double together=std::exp(-(a+b)*length);
    assert(std::abs(together-
        std::exp(-a*length)*std::exp(-b*length))<1.0e-12);
}
void SphereRayFixture()
{
    // A grazing ray that intersects the outer cloud shell, dips below
    // cloud base, and re-enters it without intersecting the solid planet.
    // Require exactly two nonempty cloud intervals, stable after camera
    // origin translations and with both contributors integrated.
    const auto intervals=BuildFixtureCloudShellGrazingRay();
    assert(intervals.size()==2);
    assert(intervals[0].b>intervals[0].a);
    assert(intervals[1].b>intervals[1].a);
    assert(intervals[0].b<intervals[1].a);
}
void HermiteCurveFixture()
{
    const std::array<HermiteKey,2> keys{{{0,0,0,0},{1,1,0,0}}};
    assert(std::abs(EvaluateCurve(keys,0.5)-0.5)<1.0e-12);
    assert(EvaluateCurve(keys,0)==0);
    assert(EvaluateCurve(keys,1)==1);
}
```

These tests specify equations and behavior, not source-extracted reference output. Add GPU-vs-CPU self-consistency checks with unit test shader variants; Vulkan validation plus RenderDoc/NSight capture for cloud buffers are required.

# Image error metrics

Visual quality is multi-dimensional; a low mean RGB error can conceal cloud geometry differences. Capture masks as well as images:
- Silhouette coverage intersection-over-union (IoU) by cloud region and distance.
- Transmittance/optical depth: MAE and p95 across sky and surface pixels; compare log(T) where T>epsilon.
- Per-pixel first cloud-depth distribution, height/top error and flow/motion vector error.
- Sunlit side luminance and sky-facing brightness error at locked exposure, separately inside cloud, clear sky, night and near the terminator.
- Sky/air optical integration coherence, halo/rim artifacts and visible seams from orbital distance.
- Temporal stability: optical difference between adjacent frames under stable time; ghost length after teleport, layer overlap and lightning.
- Shadow receiver IoU/penumbra at ground, ocean, mountains and offscreen terrains.
- Particle counts/time spectrum/audio event reproducibility and weather-to-wetness integrated amount.

Golden images from reference must include consent/licence-compatible captures; do not redistribute proprietary game textures/audio by embedding extraction into the repo. Internally generated Orbit diagnostic masks are safe to commit when they contain only independently authored synthetic scenes.

## Performance requirements

For each M scenario:
- Report cold pipeline compile, first frame, warm-cache steady state, median/p95/p99 GPU frame and pass timestamps, peak/steady VRAM and CPU simulation/submit time separately.
- Include CloudRef.FieldUpload, LightCache, ReceiverShadow, ViewMarch, TemporalResolve, Composite, Godrays, Particles, Wetness and Reflection source in profiler if enabled.
- Number of cloud-view samples per pixel, average empty-space skip distance, ray budget exhaustion %, cache hit/miss, async lag, texture faults and TAA rejection %.
- Run quality levels on both high-end and fallback non-RT Vulkan devices. Do not assume reference KSP's shader bundle performs identically on Vulkan.
- Timewarp increments cloud/light caches at a bounded frequency; the simulation may step or replay independently. Max timewarp update increase is 3 in the shipped EVE config, but actual GPU scheduling implementation is Orbit's responsibility.
- Accept an optimization only if *combined* GPU cost (cache+march+resolve+shadow+other overhead) improves while image and correctness errors stay within agreed limits.

# Implementation-phase acceptance (NO skipping)

- **F0 provenance:** all shipped modules catalogued; license and omitted maps enumerated; cannot claim exact shader source. Docs complete.
- **F1 importer:** typed ConfigNode parser and private asset resolver; 19/70/27 inventory, 293 curve keys retained; strict diagnostics.
- **F2 frozen source contract:** GPU snapshot with body/revision/epoch, GPU fences, geometry/texture validation and independent renderer readiness. Single-view synthetic tests pass.
- **F3 density/reference smoke:** independent 3D medium compiles; cloud and dust/fog/gas giant geometry; coverage/type map and 2D-orbit handoff smoke.
- **F4 direct/sky lighting:** atmosphere LUT, solar disc, terrain occlusion, Sun volume and sky SH, shadow receiver, no double multiplication.
- **F5 FX and surface:** particle fields, weather lightning, sound events, glass droplets, wetness and ocean reflection.
- **F6 temporal/multi-view:** independent history, overlap disocclusion, timewarp, two planets, reflection captures, device reset, 16K.
- **F7 visual matching:** golden scene screenshots with complete legal reference assets, optical/statistical tolerances and reference-specific tuning.
- **F8 adoption:** only then expose Ready (not Preview) under Cloud renderer. A/B remain fully operational rollback. Measure performance and user signoff.

# Mandatory missing dependencies / no-go conditions

- No source shader from this ZIP: prevents line-for-line exact port.
- 103 omitted referenced cloud texture/sound logical paths: prevents reference image parity.
- Unverified Unity/KSP render callback and shadow/atmosphere internal stage ordering: requires public relevant version source or GPU trace.
- Unity compiled shader bundles cannot be linked as Vulkan shader modules; decompilation/distribution permission should not be presumed.
- Released license all-rights-reserved; never commit packages or derivatives containing proprietary code or protected asset contents to this public repo.

These should be reported prominently in the existing Debug Center as blockers, not hidden by an unrelated successful synthetic cloud render.
