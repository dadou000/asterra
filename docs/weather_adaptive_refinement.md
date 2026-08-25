# Adaptive atmospheric refinement

Asterra's weather direction is a moving nested hierarchy rather than one uniform high-resolution planet.

| Level | Target horizontal cell | Target vertical levels | Typical moving domain | Purpose |
|---|---:|---:|---:|---|
| L0 planet | 20 km | 30 | whole planet | jets, cyclones, fronts, drylines, synoptic setup |
| L1 disturbance | 5 km | 48 | 1100 km | mesoscale boundaries, cap erosion, convective opportunity |
| L2 convection | 1 km | 64 | 420 km | explicit thunderstorms, cold pools, supercells |
| L3 storm | 250 m | 84 | 120 km | mesocyclone, storm splitting, RFD/FFD structure |
| L4 tornado | 75 m | 112 | 30 km | near-ground vortex stretching and tornado-scale flow |

## Current implementation status

`scripts/weather/weather_refinement_manager.gd` is live and performs the control-plane portion of the hierarchy:

- scans the live global atmospheric products every 0.5 real seconds;
- scores environmental opportunity separately from existing cloud/rain so capped blue-sky severe setups can refine before initiation;
- consumes native ascent, downdraft, upper-ice and wind diagnostics when the native backend is available;
- extracts spherical candidates with tile maxima plus geographic non-maximum suppression;
- tracks moving disturbances and leads the domain slightly downstream;
- applies promotion/demotion hysteresis and minimum level lifetimes;
- merges strongly overlapping domains and enforces an active-region budget;
- exposes region snapshots and `resolution_at_direction()` to future native child solvers;
- draws the requested moving footprints on the interactive WeatherMap globe.

The currently compiled L0 solver is still 256x128 with six vertical layers (~86 km at the equator). The 20 km / 30-level L0 and dynamic L1-L4 numerical solvers are the next implementation stage. The planner intentionally caps coarse-parent requests at L2. L3/L4 must be promoted from actual child-domain storm metrics so tornado resolution is never selected from a coarse global proxy alone.

## Refinement philosophy

Refinement follows physical state, not the player. A disturbance may therefore receive fine simulation thousands of kilometres away if its atmosphere warrants it. Child domains will use parent boundary forcing with a sponge rim, and later two-way feedback will return spatially averaged heat, momentum, moisture and pressure tendencies to their parents.

The hierarchy must support multiple simultaneous domains, moving domains, overlap merging, explicit compute budgets and deterministic scheduling. Numerical state must survive domain movement/regridding rather than respawning weather when a box recentres.
