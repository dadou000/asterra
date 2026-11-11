+++
path = "/rendering/clouds/references/blackrack-eve-orbit-design"
title = "Applying EVE Release 5 techniques to Orbit's cloud architecture"
kind = "concept"
status = "research"
owner_module = "OrbitCelestialClouds"
summary = "Code-oriented architecture and measurement plan for improving Orbit's 20 ms cloud raymarch using evidenced EVE Release 5 principles, with quality gates and safe fallbacks."
keywords = ["cloud optimization", "cloud A/B", "optical depth", "light volume", "temporal reconstruction", "cloud layers", "checkerboard", "cloud performance", "sun depth", "weather"]
sources = ["engine/celestial_clouds/src/CloudRenderer.cpp", "engine/celestial_clouds/include/orbit/celestial_clouds/CloudRenderer.hpp", "engine/studio_ui/src/StudioViewportRenderer.cpp", "engine/studio_ui/src/StudioViewportAtmospherePass.cpp", "engine/celestial_clouds/docs/raymarch.md", "engine/celestial_clouds/docs/shadows-and-light-volume.md", "tools/orbit_ab.py", "tools/check_cloud_shaders.py", "tests/CloudLightVolumeTests.cpp"]
symbols = ["CloudRenderer::Draw", "CloudRenderer::UpdateLightVolume", "CloudRenderer::DrawShadow", "CloudRenderer::Resolve", "CloudRenderer::SetShaderDefines", "AltitudeSunTau", "VolumeTau", "VolumeLighting", "FarFieldTransmittance", "kMaxSteps", "Extinction"]
related = ["/rendering/clouds", "/rendering/clouds/raymarch", "/rendering/clouds/shadows-and-light-volume", "/rendering/clouds/references/blackrack-eve-release5"]
depends_on = ["/rendering/clouds"]
[routes]
"how to benchmark cloud performance before implementing" = "/rendering/clouds/references/blackrack-eve-orbit-design"
"how to reuse sunlight and cloud ambient" = "/rendering/clouds/references/blackrack-eve-orbit-design"
+++

# Motivation: measured GPU bottleneck and scope

One user-provided Orbit trace from 2026-10-13 showed approximately 24.52 ms mean GPU frame time and 20.56 ms mean <view>.Clouds time, median 20.23 ms, 95th percentile 24.43 ms (320 GPU samples). This is a scene-specific baseline, NOT a general expected cost, and the trace contains no per-shader-function attribution. The cloud pass was about 84% of measured frame time. Atmosphere ~2.16 ms and SharedDirectLighting ~0.44 ms in that scene. Repeated per-sample sun depth and procedural noise are strong code-level suspects but not measured independently.

We are borrowing *principles* from the closed Release 5 distribution and public wiki, not copying its shaders or pretending to know its private source. Provenance and limits: /rendering/clouds/references/blackrack-eve-release5.

# The actual Orbit renderer as inspected on Harmony

engine/celestial_clouds/src/CloudRenderer.cpp embeds the HLSL. Key shipped compile-time defaults:
- CLOUD_MAX_STEPS=320; CLOUD_SCHEDULE_STEPS=140; CLOUD_BASE_STEP=60.0; CLOUD_EMPTY_SKIP=4; CLOUD_EMPTY_COVERAGE=1e-4.
- CLOUD_FAR_EARLYOUT=1.
- CLOUD_SUN_VOLUME=0 and CLOUD_SUN_KNOTS=0: *in the default main march*, for each nonempty participating sample the shader uses AltitudeSunTau(position,sun,16). This traces up to 16 secondary altitude slabs and calls ColumnData and Extinction again.
- CLOUD_CHECKERBOARD=0: march the chosen cloud target without additional interleaved phase reduction.
- CLOUD_PROF_CHEAPNOISE, CLOUD_PROF_NOSUNDEPTH, CLOUD_PROF_NOOCTAVES, CLOUD_PROF_NOAERIAL exist for ablation tests.
- CLOUD_PROF_STEPVIS provides a step-count heatmap. CLOUD_SUN_KNOTS enables interpolated 350 m sun-depth knots. CLOUD_SUN_VOLUME tries the existing light-volume cache, falling back to altitude slabs.
- Extinction wraps ExtinctionProcedural and optional simulated-storm sampling. A 32^3 tileable RGBA noise field is reconstructed with trilinear taps; multiple cloud components can request separate noise evaluations.
- Main-ray adaptive sampling shrinks dense intervals to 15..175 m; per-pixel growth schedule accounts for distance and footprint; empty skips have rollback to avoid skipping a previously missed edge.
- Four multiple-scattering approximations and an optional 16-step camera-to-cloud aerial perspective are computed for cloud pixels.
- Cloud light volume: three camera-relative toroidal cascades at 250 m, 2 km and 8 km spacing; updated in a separate pass, with versions/identity tags and a skip-on-settled-input fingerprint.
- CloudShadow is a distinct reduced-resolution 16-step receiver-to-star pass feeding shared direct lighting. Terrain visibility is separately represented; not every main-ray cloud sample necessarily performs a 512-step terrain-sun query.
- CloudRenderer::Draw writes premultiplied scattered radiance RGB and transmittance A; CloudsResolve temporally reprojects/clamps; CloudsComposite blends the result after Atmosphere.
- Studio per-view settings: cloud_resolution_scale (default 0.5, range 0.25..1), cloud_temporal, cloud_light_volume, cloud_godray_strength, bypass_cloud_shadow, and cloud lab. The renderer documentation says only the first near-field cloud layer is drawn; test the current code before designing multiple-layer refactors.

Contrast with EVE Release 5: both have separate cloud field, separate lighting representation and temporal resolve. Orbit still defaults to repeated per-sample 16-slab sun illumination whereas the blackrack Release 4+ design explicitly advises relegating LONG-distance light to the shared volume and retaining short local marches only when needed for fine detail.

# Priority zero: verify cause experimentally

Never assume CLOUD_SUN_VOLUME is faster than the baseline: its cache lookups, update and invalid fallbacks may outweigh savings or introduce history discontinuities. Same for SDF and checkerboard. Obtain controlled A/B GPU captures first.

Existing mechanisms:
- Runtime clouds.set_defines / ORBIT_CLOUD_DEFINES (CloudRenderer::SetShaderDefines): transactional shader rebuild; previous pipeline stays active on compilation failure.
- clouds.split_set: image comparison of baseline and variant with a shared cloud shadow/light volume, useful for visual integrity but not isolated total cost because both variants run in the same frame.
- tools/orbit_ab.py: project-copy, pinned camera/time, converge-and-capture, baseline repeats and difference images; verify actual GPU-pass mode before declaring timing comparisons.
- tools/check_cloud_shaders.py and tests/CloudLightVolumeTests.cpp: compile and cache-state correctness.

Suggested independent conditions, same body/time/weather, 1440p and 1080p: (A) mountain near low Sun, (B) deep cloud interior, (C) clear sky plus thin fog, (D) dense storm with lightning, (E) 5 km forward flight, (F) 500 km orbit, (G) 20s ground-to-orbit, (H) high timewarp; static, camera-moving, cloud-moving cases. Warm caches and discard startup frames, lock exposure. Run controls between variants to bound heat, clocks and noise. Keep separate graphics-queue GPU frame timing and pass timing.

Ablation matrix, one define at a time:
- baseline: current defaults.
- CLOUD_PROF_NOSUNDEPTH=1: directly upper-bounds savings from removing secondary per-sample Sun integration (image is intentionally incorrect).
- CLOUD_PROF_CHEAPNOISE=1: upper-bounds cost of detailed noise, again NOT production output.
- CLOUD_PROF_NOAERIAL=1: remove camera-to-cloud aerosol integration, check distant clouds.
- CLOUD_PROF_NOOCTAVES=1: isolate scattering-octave cost.
- CLOUD_SUN_KNOTS=1: test cached view-ray knots and sunset contouring.
- CLOUD_SUN_VOLUME=1: test volume-hit ratio, cache refresh, fallback and cloud-skin continuity.
- CLOUD_CHECKERBOARD=1 and 2: measure sample reduction plus CloudsResolve costs, motion artifacts.
- CLOUD_MAX_STEPS=160: step-pressure sensitivity; image mismatch near horizons is expected.
- CLOUD_PROF_STEPVIS=1: use a visual diagnostic to see where 320-step exhaustion happens. This is not a GPU timing mode.

For each variant record: march mean/p50/p95, CloudsResolve mean, CloudLightVolume mean, whole GPU frame mean/p95, GPU memory, output pixels, noise floor in repeated screenshots, camera/time/commit/flags, frequency and severity of rejected history, cache hit/fallback fraction. Do not sum timestamp-pass means as exact critical-path cost if the GPU profiler uses consecutive-end timestamps or overlapping work; compare full frames under identical conditions.

# Proposed staged implementation (ORBIT design, not a claim about EVE internals)

## Stage 1 — Preserve coherent math, split expensive lighting from visible shape

The rendering equation for a view-ray interval [a,b] is radiance L_out = T(a,b)*L_background + integral(T(a,t)*sigma_s(t)*source(t) dt). Shadow-light optical depth tauSun(P) = integral along direction Sun of sigma_t(Q) ds; direct stellar visibility factor TSun(P) = exp(-tauSun(P)). This is a useful reusable physical quantity to cache, with separate terrain/planet occlusion applied correctly.

A cloud primary ray is expensive because every participating sample evaluates tauSun through another (up to) 16 density samples. Define two fidelity classes:
- Fine visible extinction sigmaFine(P,footprint), preserving billows, cell boundaries, storm-derived density and conservative opacity.
- Filtered lighting extinction sigmaLight(P,lightingFootprint), attenuating sub-voxel features which cannot be resolved by the light volume or affect a long path's transmittance materially.
Require the latter to conserve integrated extinction on representative sample paths; never simply omit storm cores or cloud roof detail.

Proposed pseudocode (new Orbit algorithm, not copied code):
~~~
CloudDensityResult density = EvalCloudDensity(position, viewFootprint);
if (density.opaqueEnough)
{
    SunOpticalDepth tau = LightVolume.SampleSunTau(position);
    if (!tau.valid || tau.errorEstimate > allowedError)
        tau = TraceSunFiltered(position, localDetailRange);
    else
        tau.value += TraceLocalSunCorrection(position, localDetailRange);
    AccumulateScattering(density, tau, atmosphere, sky);
}
~~~

Important: a short-range correction must not add an overlapping segment already covered by the cached tau; express cached integral domains explicitly, e.g. split tauNear + tauFar with boundary and optical-depth correction, not add two full-range integrals. Do not blend directly between incompatible illumination models based only on camera distance: that causes nested concentric brightness terraces.

## Stage 2 — Improve existing light volume, not instantiate a second cache

Reuse CloudRenderer::LightVolume and UpdateLightVolume. Extend each voxel record or sidecar with: optical depth, optional low-order sky lighting, validity generation/tag, physical time, source fingerprint, error estimate, and optional range of integration. Keep allocations bounded and resource lifetime fenced. The current toroidal cascades and input hash are strong foundations; do not replace them with an unrelated fixed-size 224x224xN cache merely because EVE has horizontalResolution=224.

Update hierarchy:
- Sun optical depth: highest priority at visible cloud boundaries and near shadow transmittance transitions.
- Sky ambient: lower refresh priority because visual motion is less apparent (as documented by EVE).
- Newly visible or invalid voxels: high priority until filled, with robust fallback.
- Storm-near, wind-advection, lightning or strong timewarp: faster invalidation/refresh or bounded quality reduction.
- Stable far cascade: slow refresh when inputs change little.

Use angular motion of the Sun, cloud field evolution/revision, storm domain revision, temporal simulation discontinuity, camera tangent-frame re-anchor, and planet radius as keys. Reprojection from a previous volume is allowed only with validity tags and conservative atmospheric/transmittance bounds. A light-volume pass being cheap in the prior trace does not establish that raising its refresh rate is free.

## Stage 3 — Temporally distribute cloud view rays

Leverage the existing CLOUD_CHECKERBOARD modes, reduce independent raymarched pixels per frame, reproject missing samples. Maintain separate history metadata:
- world/body-fixed cloud point and shell depth interval, representative optical depth and source layer;
- advected cloud-motion vector in world space (not just camera motion);
- current and previous jitter phase and resolution;
- cloud-type/layer identity, storm revision, atmosphere revision, and timewarp epoch;
- disocclusion confidence and high-contrast silhouette/lighting change.

Proposed resolve rule [C]: history valid only if depth ordering, surface/cloud intersection, dominant layer, transmittance and reprojected motion agree within footprint-dependent thresholds. Choose new-sample priority along silhouette crossings, thin fog over dense cloud, layer overlaps, bright Sun rims, lightning flashes and newly exposed pixels. A purely screen-depth test cannot distinguish two cloud layers on one view ray.

For extreme timewarp, discontinuous simulation state or teleport, invalidate or reset accumulation. For static scenes, may amortize 4/8/9 phases; for rapid flight and thin fog, increase current-frame sampling. The EVE wiki calls 8x/9x its historical sweet spot, not a universal Orbit target. Try checkerboard=1 versus =2 first.

Screenshots and comparison captures must offer a converged/high-quality path independent of normal temporal reuse. Do not accidentally mix stale cloud history into correctness screenshots.

## Stage 4 — Meaningful empty-space skipping

The existing CLOUD_EMPTY_COVERAGE threshold of 1e-4 is close to exact zero. Nonzero weather coverage often produces visually empty regions because the high-frequency shape threshold rejects the sample. Potential new conservative representation:
- Planet weather field gives an upper bound on possible column density.
- A sparse regional tile hierarchy stores upper/lower optical-depth bounds and whether any storm simulation contributes.
- Optional column coverage-distance field (EVE-style) can skip distant disjoint cloud patches.
- Only where measured payoff exceeds update/build costs, use 3D coarse occupancy bounds over a regional storm volume.

Never treat interpolated average density as an upper bound on maximum density. Include warp, noise-erosion, high towers, procedural rain sheets, simulated storm sponge edges and temporal advection margins. Test missing-cloud risk with high-detail reference images. EVE's official SDF documentation warns that map-space SDF can be slower overall.

A sample-count heatmap and mean GPU time per step quartile should tell whether skipping beats noise-fetch overhead. Group/wave divergence can defeat asymptotic gains if nearby pixels take wildly different paths; prefer coherent tile work scheduling where possible.

## Stage 5 — Overlapping cloud layers and storm domains

Orbit currently models one near-field layer with internally mixed cloud types, plus optional simulation. EVE Release 5 chooses separate passes for overlapping layers and approximate cross-layer light-volume occlusion. For Orbit, distinguish:
1. Ordinary decks that do not overlap altitude: separate segment intervals, order by camera ray intersection.
2. Distant overlapping layers: optionally separate ordered passes with controllable quality; document possible wrong occlusion.
3. Near-camera or player-enterable storm structure: merge density/temperature/hydrometeor fields into local shared sparse samples and integrate all participating extinction in distance order.
4. Precipitation/fog with different phase functions: keep media coefficients separate if their phase behavior is materially different, but share extinction/lighting caches with typed channels.

For a two-medium overlap, transmittance must reflect tau_total = tau_A + tau_B, with in-scatter contributions evaluated per medium; simply multiplying two independent premultiplied cloud layers using fixed object order cannot reproduce the general result.

Avoid increasing every frame's draw-pass count for all planetwide storms; visibility and distance partitioning need to bound active complexity.

## Stage 6 — Keep surface, atmosphere, water and orbital rendering coherent

The shared cloud field must determine optical thickness consistently for near field, cloud shadows, light volume, and orbital representation. Direct sunlight through atmosphere = stellar irradiance * atmospheric transmittance * planet/terrain disc visibility * exp(-cloud optical depth). Apply cloud attenuation exactly once to each distinct physical path. Atmospheric air in-scattering affected by cloud shadow should agree with ground shadows, not use an unrelated screen darkening overlay.

Where surface water or mirrors reflect clouds, select reflected radiance from the same representation or a temporally stable lower-detail fallback. Reflection probes must contain clouds and use consistent exposure/sky radiance. At orbit distance, near-cloud raymarch resolution eventually becomes insufficient: match optical thickness and normal-dependent lighting when transitioning to a coarser appearance, and make transition continuous with altitude and projected footprint.

Helion and Asterra need configurable angular stellar radius, eclipse visibility, planet curvature and local tangent basis. The cloud renderer cannot assume KSP's Kerbin scale, Earth radius, or a fixed cloud layer altitude.

## Stage 7 — Weather generation and timewarp contracts

Do not regenerate the entire cloud dataset because the camera moves. Orbit already separates global weather, active local storms, and replay/precomputed states. Maintain distinct tick clocks for climate, zone emergence, storm physics, cloud representation refresh, light-volume refresh, and rendered frames. Under timewarp, update all observed states from absolute simulated time; skip intermediate physics steps or switch to replay as designed, but invalidate lighting and temporal history on discontinuities.

For storm life cycles, preserve updraft, overshooting top, anvil, flanking line and precipitation core as large-scale density constraints; fine-noise erosion must not erase meteorological structure. EVE's painter-tile intermediate scale offers a useful authoring analogy, but Orbit should bake its global conditions/zone field or authorable storm blueprints, not require satellite textures to animate a storm.

# Proposed safe implementation sequence and acceptance gates

P0: DO NOT modify production output. Add a dedicated, saveable profiler experiment preset invoking the current flags; capture control/variance plus static and moving cameras. Record commit SHA and GPU timing capability.
P1: Measure CLOUD_PROF_NOSUNDEPTH, CLOUD_PROF_CHEAPNOISE and CLOUD_PROF_NOAERIAL; choose dominant cost with measured attributable *difference*, not theoretical max loops.
P2: Enable/test existing CLOUD_SUN_KNOTS with stable density and high-contrast sunlight. Quantify raymarch time delta, near-cloud fidelity, terminator rings and temporal noise.
P3: Enable/test CLOUD_SUN_VOLUME. Compare cache-hit fallback, offscreen storm shadows and cloud-skin stability. Integrate a bounded near correction only if the data justifies it.
P4: Test checkerboard 1 and 2 including history disocclusion, lightning, thin fog, overlapping clouds and flight. Compare total GPU time and image difference after convergence.
P5: Introduce conservative empty-column / region bounds behind a flag, then optionally coarse 3D occupancy for sparse storm zones. Maintain exact procedural fallback.
P6: Enhance sky ambient volume and cross-layer visibility only after P1-P5 profiles and shadow correctness tests.

Acceptance gates for any chosen production variant:
- no missing wisps, tower tops or high cirrus when camera or Sun moves;
- no repeated annular bands at planetary terminator or cache cascade boundaries;
- stable cloud-depth ordering at fog/overlap intersections, and no dark/bright halos from invalid history;
- actual integrated sunlight extinction matches reference paths within an agreed tolerance across dawn/sunset/midday;
- no history carried across discontinuous timewarp or world replacements;
- readback/screenshots include exact running version, camera, cloud flags and exposure;
- 95th percentile cloud frame time improves in the target scene while capture image error remains acceptable; fixed GPU-memory budget on low-end hardware.

# Diagnostics and profiling instrumentation specification

Counter/timer proposals:
- viewRayStepsExecuted, viewRaySamplesNonzero, viewRayBudgetExhaustedPixels;
- proceduralDensityFetches, secondarySunIntegrations, secondarySunDensitySamples;
- lightVolumeValidHits, fallbackQueries, generationMisses, updatedVoxels, staleVoxels;
- emptyRegionSkips, rejectedPotentialSkips, nearCorrectionQueries;
- temporalFreshPixels, rejectedHistoryPixels, rejectionReason histogram;
- per-pass GPU duration and p95 from <view>.Clouds, CloudsResolve, CloudLightVolume, CloudShadow, Atmosphere.

Sampling per pixel with global atomics could dwarf the rendering cost; aggregate per workgroup or sparsely sampled debug tiles into a small readback buffer, amortize readback, and disable counters for baseline production comparisons. Expose experiment metadata in the unified Debug/Profiler UI rather than separate launchers. Use profiler capture, not image screenshots, when the action says performance capture.

When comparing variant times, show the change relative to an interleaved reference measured in the same session/conditions; do not promise percentage savings before observing it.

# References

- Evidence and archive inventory: /rendering/clouds/references/blackrack-eve-release5.
- Orbit cloud march: /rendering/clouds/raymarch.
- Orbit light volume: /rendering/clouds/shadows-and-light-volume.
- EVE Release 5 technical summary: https://www.patreon.com/blackrack/posts/true-volumetric-139879553
- EVE light volume docs: https://github.com/LGhassen/EnvironmentalVisualEnhancements/wiki/Light-volume-(Release-4)
- EVE temporal upscaling: https://github.com/LGhassen/EnvironmentalVisualEnhancements/wiki/Temporal-upscaling-and-noise-detiling
- EVE overlapping layers: https://github.com/LGhassen/EnvironmentalVisualEnhancements/wiki/Overlapping-layers(Release-5)
- EVE SDF caveat: https://github.com/LGhassen/EnvironmentalVisualEnhancements/wiki/Signed-Distance-Fields-(Release-4)
