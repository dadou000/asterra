+++
path = "/world/weather-lab/global-system-status-p9"
title = "Storm domain: measured status of phase 9 (close-range quality)"
kind = "concept"
status = "experimental"
summary = """
Phase 9 of the storm domain plan, defined after phase 8: the storm looked right from 60 km and plastic from 13 km. A detail pass in the cloud shader (displaced sampling, smoothstep interpolation of the simulation, a continuous multi-octave carve of the fringe) gives the close view a cumulus-like silhouette; what was tried, what failed, and the measurements."""
keywords = ["close range", "storm detail", "domain warp", "carve", "smoothstep interpolation", "stair artifact", "CLOUD_STORM_DETAIL", "phase 9"]
sources = [
  "engine/celestial_clouds/src/CloudRenderer.cpp",
]
symbols = ["CloudRenderer"]
invariants = [
  "The detail pass fades out as a pixel grows wider than its features, so the far view of a storm is the smooth simulated density; CLOUD_STORM_DETAIL=0 removes it (and the smoothstep interpolation) for A/B work.",
  "The carve is a continuous modulation, strong in the thin fringe and absent in the dense core; a hard threshold left isolated flecks and holes, and linear interpolation of the simulation shows its cell planes as a stair of one-cell steps once the fringe is carved.",
  "The surface a viewer sees is where the line of sight has gathered an optical depth of about 1, which for a thick storm is well inside the thin fringe; the carve must reach that deep (CLOUD_STORM_CORE) or the finest octaves land in the smooth core.",
  "The interior of the storm is still smooth: the 500 m grid is the limit of what the simulation provides, and the detail pass only shapes the surface and the fringe.",
  "Results are single runs of one storm on one machine.",
]
related = ["/world/weather-lab/global-system-status-p8", "/rendering/clouds/raymarch", "/world/weather-lab/gpu-port"]
depends_on = ["/world/weather-lab/global-system-status-p8"]
verify = ["place a storm (storm.place) and view it from 13 km at several pitches; compare with clouds.set_defines CLOUD_STORM_DETAIL=0", "far view of the same storm: the two should match within the capture noise"]
verified = "5aec831"
+++

**P9 (2026-10-12): close-range quality; first slice.** Every capture so far had been from 60 to 320 km, where softness hides. From 13 km the nested storm was a smooth, featureless grey bloom with a silky outline: the simulation gives the large-scale shape, but the 500 m grid and a single weak erosion factor give nothing at the scale a close viewer reads.

What was changed (all in `StormSigma`, `CloudRenderer.cpp`, tunable at run time through `clouds.set_defines`):
1. **Displaced sampling** (`CLOUD_STORM_WARP`, default 500 m): the simulation is sampled at the position plus a low-frequency noise vector (380 m scale), so surfaces billow at sub-cell scale; it fades out when a pixel is wider than about 150 m.
2. **Smoothstep interpolation of the simulation** (parent and child): with plain trilinear weights the carved surface followed the cell planes as a regular stair of one-cell (500 m) steps.
3. **A continuous multi-octave carve** (600, 250 and 100 m): the density is modulated by up to 90 % in the thin fringe and not at all in the dense core (`CLOUD_STORM_CORE`, default 0.12 /m, `CLOUD_STORM_CARVE`, default 1).

What failed on the way, and why it matters:
- A first carve scaled by `sigma / 0.02` changed little: the visible surface of a thick cloud is where the line of sight has gathered an optical depth of about 1, which is deeper than the thin fringe, so the carve has to extend to much higher densities (0.12 /m).
- A hard threshold carve with 110 m and 40 m octaves made scattered bright flecks and dark holes, like speckle; the continuous modulation at larger scales replaced it.
- Stronger warp alone only made the outline lumpier at a larger scale.

| Check | Result |
| --- | --- |
| Close view (13 km), looking up at the storm | the silhouette is irregular and frayed like a cumulus fringe, with tufts and a lit edge; before it was a smooth outline |
| Far view (70 km), detail on against off | mean difference 0.087/255 (the capture noise is 0.05 to 0.07), 2266 pixels over 12/255; the storm looks the same from far away |
| Clouds pass, close view | about 6.2 to 6.6 ms with the detail against 6.2 without (the profiler window repeated a stale value, so no finer claim) |

- Not done: interior structure (the simulation's cells set it), rain shafts and a visible cloud-base structure, a second warp octave, tuning of the 500/0.12/1 defaults on more than one storm and one camera, any treatment of the anvil underside (mammatus), lightning driven by the simulated storm.
