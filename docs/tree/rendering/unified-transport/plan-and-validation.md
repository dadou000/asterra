+++
path = "/rendering/unified-transport/plan-and-validation"
title = "Implementation order, current-code corrections and validation (status tracked below)"
kind = "playbook"
status = "experimental"
summary = """
Phased order for adopting the unified transport architecture, the near-term corrections to existing cloud, haze, light-overflow, glass and caustic code, \
and the benchmark scenarios and error metrics every step must report."""
keywords = ["roadmap", "phases", "benchmark", "validation", "acceptance", "scenarios", "corrections"]
related = ["/rendering/unified-transport", "/rules/completion-check"]
depends_on = ["/rendering/unified-transport"]
invariants = [
  "Corrections ship incrementally and are benchmarked independently; each reports performance saving and introduced error.",
  "Accuracy is measured in linear HDR radiance, transmission, silhouette, temporal stability and shadow visibility, not tone-mapped output alone.",
  "Every visual test has a higher-quality reference path (accurate volumetric and solar-disk integration for clouds, exact geometry for glass, the same event timeline with expensive scattering for lightning).",
  "Sophisticated cloud, lightning and refraction features consume the common optical and transport infrastructure rather than add isolated approximations.",
]
+++

## Status by phase (HEAD 91fceb6c)

Foundation: CPU data structures only, no shared medium service. Cloud correctness: far field behind a flag. Optical hierarchy, atmosphere coupling, world-space
transport, visual cloud LOD: not wired. Light hierarchy: city tree and limb darkening built, partly wired. Lightning simulation and transient lighting: built and
reachable over RPC/MCP, higher-order response missing. Glass: pre-pass behind a flag. Refraction: sun only. Validation scenarios: none run.

## Phases

| Phase | Work | Acceptance |
| --- | --- | --- |
| Foundation | shared coordinates, units, medium authority, versions, diagnostics | all consumers agree on extinction and source state |
| Cloud correctness | stable morphology, optical filtering, marcher repair | approaching clouds reveals detail without changing identity |
| Optical hierarchy | sparse occupancy, empty-space skipping, directional transmission | major cut in redundant density and sun queries |
| Atmosphere coupling | dynamic haze and fog, cloud/terrain consistency | one atmospheric state gives consistent visibility |
| Light hierarchy | source aggregation, bounded selection, stellar disks | cities and multiple stars light clouds without source-count explosion |
| World-space transport | cache dependencies, incremental scattering, surface feedback | camera movement does not change the lighting solution |
| Visual cloud LOD | curved 2D, 2.5D, 3D promotion | unnoticeable orbit-to-cloud transitions |
| Lightning simulation | charge fields, branching graph, channel memory | convincing intracloud, intercloud and ground discharge |
| Transient lighting | event injection, compressed higher-order response | large flashes cause no unbounded GPU spikes |
| Glass restructuring | shared visibility, photon compute, adaptive caustics | glass cost scales with actual contribution |
| Atmospheric refraction | planetary LUT, local gradients, masked visibility | correct mirages and horizon effects at controlled cost |
| Experiments | wavefront marching, reservoirs, spectral compression, neural residuals | adopt only if they beat simpler baselines |

## Corrections to existing code

Cloud shape blending toward coverage with distance; separate near/far extinction multipliers; differing view and solar density footprints; possible
interval/jitter inconsistency in the cloud sample position (fix the integration interval before choosing the sample); repeated light-volume integration
toward the star; fixed Mie altitude profiles for haze; private cloud aerial perspective; single dominant star assumption; tiled local-light overflow
dropping later lights by assignment order (replace with importance selection or energy-preserving aggregation); glass double environment search; software
exact-triangle traversal in glass; per-vertex photon tracing in caustics; caustic resolution chosen by physical size; full-resolution backdrop copy;
distortion-only refraction without masked offscreen fallback.

## Required scenarios

Orbit-to-ground descent through clouds; cumulonimbus approached from several angles; hidden cloud shadowing a visible cloud; Sun above the astronomical
horizon but behind a mountain; two stars with different colours and sizes; night city under low overcast; humid clear day versus dry dusty day; internal
supercell lightning; 300 km storm complex with secondary discharges (bounded workload, charge conservation); strong inversion over ocean or road; thick
glass sphere versus window; caustics under stable sunlight; extreme timewarp across weather systems (no backlog, deterministic); camera cut and
floating-origin transition (cache ownership and invalidation).

## Scenario status (2026-10-11, HEAD 8a4a98db; on-screen unless noted)

| # | Scenario | Status |
| --- | --- | --- |
| 1 | Orbit-to-ground descent through clouds | Partly: 1590 km to 6 km keeps the same clouds with the filtered far field (`/rendering/unified-transport/design-review`). Not run through a deck. |
| 2 | Cumulonimbus from several angles | Not run. |
| 3 | Hidden cloud shadows a visible one | By construction only: light-volume voxels are pure functions of position and sun. Not run with two clouds. |
| 4 | Sun behind a mountain above the astronomical horizon | Not run. |
| 5 | Two stars | Gap in code: `DominantDirectLightingAtBody` keeps only the strongest emitter, and every pass reads one `directionBody`. |
| 6 | Night city under overcast | Gap: `CityLightTree` is not on the live light path and the world has no city content. |
| 7 | Humid clear vs dusty dry | Gap: aerosol growth (`HygroscopicGrowthFactor`) has no consumer; haze is the fixed Mie profile. |
| 8 | Internal supercell lightning | Partly: authored and flash lights reach the cloud march (checked in a daytime cloud); the night look is not verified. |
| 9 | 300 km storm complex, bounded workload | Not run; the cloud pass takes at most 16 lights. |
| 10 | Strong inversion mirage | Gap: only the planetary sun/sky table exists. |
| 11 | Thick glass sphere vs window | Gap: no thin-pane path. |
| 12 | Caustics under stable sun | Partly: compute pre-pass exists; no photon reuse across frames. |
| 13 | Extreme timewarp | **Pass.** Same absolute time gives the same image in two launches (0.03/255 at +30 days, +1 year, +10 years), different times give different weather (21 to 47/255 apart), frame time stays 6 to 11 ms after a 10-year jump (`tools/ab_specs/timewarp.json`). The first scene after launch needs the weather pre-spin (about 45 s). |
| 14 | Camera cut and floating origin | Partly: after a 1590 km to 25 km jump the frame is 13.6/255 from converged at 1 s and 0.07 at 8 s, identical with the light volume off, so it is not the volume; the cause is not isolated. |

Corrections table: the glass "backdrop copy when no glass is visible" item is not a gap. With the glass sphere out of view none of the glass passes
appear in the GPU pass list (they are culled with the instances). The Clouds march is the dominant frame cost in sky views (about 9 of 13 ms in a
single profiler snapshot). Changing its step constants does not help without thinning distant clouds (review `R-20261011-181752-8a4a98db`);
the spec's hierarchical empty-space skipping is the way to cut it. The cost attribution (review `R-20261011-190655-8f9c9165` series, step heat map) showed empty sky pixels run 110 to 224 of 224 march steps, no per-stage or per-column test frees any of it, and an exact height-band leap made the pass slower, so the next lever is temporal reuse of the march rather than a hierarchy (see the raymarch block).

Measure per-pass GPU time, density evaluations, ray steps, BVH visits, cache-hit ratios, invalidated bricks, memory traffic and background compute.
