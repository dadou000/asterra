# Weather core replacement execution plan

The old latitude/longitude solver is not a supported fallback. Git history is the rollback mechanism. The replacement is implemented directly on `weather/0.0.5`; compatibility shims are temporary build scaffolding only and are deleted as soon as the new runtime core is usable.

## Non-negotiable invariants

Every accepted timestep must satisfy all of these. A violation rejects the timestep and retries with a smaller dt; it is never hidden by clipping the state.

- no NaN or Inf;
- dry layer mass > 0 everywhere;
- every water species >= 0;
- exact shared-edge equal/opposite mass fluxes;
- closed dry-mass drift < 1e-7 per simulated day in closed tests;
- closed total-water drift < 1e-6 per simulated day;
- horizontal CFL target <= 0.45;
- no 2-delta-x pressure/velocity computational mode;
- no pole-specific numerical treatment because the grid has no coordinate pole;
- no artificial target-jet/travelling-wave momentum forcing in production physics;
- all atmosphere <-> surface water transfers use physical kg/m2 and close exactly;
- diagnostic/render values never feed back as conserved physics variables.

## Phase 0 - Geometry and test infrastructure [IN PROGRESS]

Deliverables:
- `CubedSphereGrid`, 6 x 256 x 256 gnomonic faces at Asterra R=3500 km;
- double-precision centers, areas, edge lengths, tangent bases and edge normals;
- reciprocal seam/corner connectivity;
- standalone C++ tests independent of Godot;
- CI runs tests before building the GDExtension.

Gates:
- summed cell area equals 4*pi*R^2 to numerical precision;
- every neighbour relation is reciprocal;
- both sides of every physical edge agree on edge length;
- shared-edge conservative flux sum closes to round-off;
- no degenerate cells or singular metrics.

## Phase 1 - Conservative 2-D transport

Implement a finite-volume scalar transport core on the cubed sphere.

State:
- cell mass / layer thickness scalar;
- passive tracer mass;
- one shared normal flux per physical edge.

Numerics:
- first-order donor cell reference implementation;
- MUSCL reconstruction with monotonized-central limiter after reference passes;
- SSPRK3 integration;
- adaptive CFL controller and rollback.

Gates:
- uniform field remains uniform;
- solid-body tracer rotation crosses every face seam;
- global scalar mass drift meets invariant;
- positivity preserved without post-step clamp;
- first-order and MUSCL solutions converge with resolution.

## Phase 2 - C-grid shallow-water dynamics

State:
- layer mass at cells;
- normal momentum at shared edges.

Derived:
- geopotential;
- divergence;
- vorticity.

Implement pressure-gradient and Coriolis operators in flux form. Scalars and velocity are staggered, removing the current A-grid checkerboard null mode.

Gates:
- lake/rest state remains at rest;
- balanced solid-body/zonal flow remains balanced;
- Rossby-Haurwitz/barotropic wave test remains bounded;
- no cube-face/corner imprint grows with time;
- no numerical polar vortex because there is no pole singularity.

## Phase 3 - Replace `WeatherNative` runtime

At this milestone the old latitude/longitude runtime is deleted, including:
- `weather_native_oklahoma.cpp` wrapper;
- latitude/longitude polar filters;
- empirical perturbation-pressure prognostic equation;
- Shapiro pressure checkerboard repair;
- prescribed travelling target wind waves;
- non-conservative vertical donor/receiver exchange.

The GDExtension exposes the new cubed-sphere core directly. Globe/render output is resampled from the cubed sphere to the existing 1024 x 512 display texture only as a presentation operation.

## Phase 4 - 30-level dry hydrostatic atmosphere

Vertical coordinate:
- 30 hybrid sigma-pressure layers;
- prognostic dry mass / surface pressure;
- hydrostatic pressure and geopotential diagnosed from layer mass and virtual temperature.

Conserved state:
- dry mass;
- mass-weighted thermodynamic variable;
- edge-normal momentum.

Numerics:
- same shared horizontal mass flux transports all cell quantities;
- conservative vertical remap/mass exchange;
- SSPRK3 outer integration;
- adaptive timestep / rollback;
- scale-selective high-order diffusion only at the shortest wavelengths.

Gates:
- resting hydrostatic atmosphere remains at rest;
- balanced zonal jet test;
- Jablonowski-Williamson / Polvani baroclinic wave;
- Held-Suarez >= 1000 simulated days bounded;
- dry mass and energy diagnostics remain within gates.

## Phase 5 - Surface energy coupling

Add terrain, water fraction, albedo, soil/snow and surface temperature back after the dry atmosphere is stable.

Rules:
- every sensible/latent energy transfer appears with equal/opposite atmospheric/surface accounting;
- no state reset when surface data arrives;
- surface update uses the same accepted timestep as the atmosphere;
- budgets are accumulated in double precision.

Gates:
- global energy residual reported every step;
- closed no-radiation exchange test conserves combined energy;
- surface spin-up cannot create atmospheric mass or water.

## Phase 6 - Water vapour only

Add vapour as conserved water mass, transported using exactly the dry-air mass flux.

Gates:
- passive moist tracer rotation;
- evaporation surface -= dm, atmosphere += dm exactly;
- no negative vapour without clipping;
- total atmosphere+surface water closes.

## Phase 7 - Reversible cloud microphysics

Add liquid and ice with only reversible phase changes first.

Rules:
- condensation: vapour -= dm, condensate += dm;
- evaporation: condensate -= dm, vapour += dm;
- freeze/melt transfers phase only;
- latent energy exchange explicitly accounted;
- no optical/cloud-coverage heuristic feeds back to water mass.

Gates:
- isolated parcel phase-cycle returns to original total water/energy;
- multi-day global moist run has no monotonic condensate drift.

## Phase 8 - Precipitation and surface hydrology

Add rain/snow hydrometeor mass and sedimentation.

Rules:
- atmospheric fallout `dm` is exactly the `dm` deposited to the surface;
- mm/h is derived from kg/m2/s for UI only;
- runoff/ocean transfer is an explicit reservoir transfer.

Gates:
- storm-column water budget closes;
- global atmosphere+surface water budget closes over long runs;
- precipitation cannot exceed available hydrometeor mass.

## Phase 9 - Physical moist convection

Add convection only after resolved moist dynamics is stable.

Replace the current asymmetric vertical mixer with conservative plume/mass-flux transport:
- a single interface mass flux determines donor mass;
- transported vapour, condensate, heat and momentum use that same donor mass;
- entrainment/detrainment are explicit equal/opposite transfers;
- CAPE consumption and latent heating are diagnosed/accounted.

Gates:
- zero-CAPE column remains inactive;
- convective adjustment closes water and enthalpy budgets;
- no domain-wide activation caused by grid-scale divergence noise.

## Phase 10 - Severe and tropical regimes

Add severe-weather and tropical closures one at a time. They may redistribute conserved state but may not inject unaccounted mass/water/momentum.

Each closure gets:
- an isolated deterministic unit test;
- a budget test;
- a multi-day idealized environment test;
- a maximum allowed tendency independent of rendering clamps.

Only after these pass do we tune Oklahoma-like sparse-cloud severe climatology.

## Phase 11 - L1+ nonhydrostatic nests

Build a separate tangent-plane C-grid nonhydrostatic core for ~5 km and finer domains.

- L1 ~5 km / 48 levels;
- L2 ~1 km / 64 levels;
- L3 ~250 m / 84 levels;
- L4 ~75 m / 112 levels.

Use split-explicit fast-wave substeps. Parent/child boundary and feedback transfers are conservative in mass, water, heat and momentum.

## Runtime diagnostics required in the `é` menu

Always expose:
- accepted dt;
- max CFL;
- rejected step count;
- dry-mass drift;
- total-water drift;
- energy residual;
- axial angular momentum drift;
- min layer mass;
- min water species;
- max wind;
- NaN/Inf count;
- limiter count;
- current solver revision/backlog.

Any invariant failure is displayed prominently and freezes further physics after rollback if reducing dt cannot recover safely.
