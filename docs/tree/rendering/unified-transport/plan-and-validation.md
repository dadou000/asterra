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

Measure per-pass GPU time, density evaluations, ray steps, BVH visits, cache-hit ratios, invalidated bricks, memory traffic and background compute.
