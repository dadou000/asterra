# Asterra weather dynamical-core rewrite plan

## Decision

Do not keep tuning the current 1024x512 latitude/longitude solver. Replace the L0 dynamical core with a conservative hydrostatic finite-volume C-grid on a 6-face gnomonic cubed sphere. Keep the current solver only as a visual/reference implementation until the replacement passes the tests below.

At Asterra radius 3500 km, a 6 x 256 x 256 cubed sphere has essentially the agreed ~21.5 km nominal L0 spacing while using 393,216 horizontal cells instead of 524,288 latitude/longitude cells.

L1 and finer nests remain separate moving tangent-plane domains. At ~5 km and below they must use a non-hydrostatic C-grid core; do not inherit the hydrostatic L0 equations blindly.

## Why the current core is structurally unstable

1. **Pole topology is wrong.** Every latitude ring owns 1024 independent longitudes even though physical zonal spacing tends to zero. The current hard `cos(latitude) >= 0.10` metric cap and post-step polar smoothing are not a reduced grid and produce latitude-ring artifacts.
2. **A-grid pressure null mode.** Pressure, u and v are collocated. Central pressure gradients use p(i+1)-p(i-1); an alternating +/- checkerboard therefore has exactly zero gradient. The post-step Shapiro filter masks this mode but cannot remove the null space that creates it.
3. **Pressure is not tied to atmospheric mass.** Each level carries a free perturbation-pressure scalar with empirical thermal, moisture and divergence tendencies. A hydrostatic global core should derive pressure/geopotential from prognostic layer mass/surface pressure and thermodynamic state.
4. **Horizontal transport is not conservative.** Bilinear semi-Lagrangian interpolation is applied independently to q, condensate, pressure and momentum. It can drift total tracer mass and does not use the same face mass flux for all tracers.
5. **Vertical exchange is explicitly non-conservative.** The current donor/receiver fractions differ. For an upward exchange, one level can lose 0.25*f*delta while the other gains 1.0*f*delta. The severe wrapper adds more `upper_gain < 1` exchanges. This creates/destroys water, heat and momentum.
6. **Condensate lofting destroys water.** Some liquid lofting adds only 82% or 88% of the removed amount to upper ice.
7. **Precipitation is dimensionally disconnected from atmospheric mass.** Condensate mixing-ratio fallout is converted to a 0..1 diagnostic, then the surface interprets that diagnostic as an independent kg m^-2 s^-1 rain rate. Atmosphere -> surface water is therefore not a closed transfer.
8. **Hard clamps hide instability.** Wind, pressure, theta, q and condensate are clipped after tendencies. A stable core should preserve positivity/conservation by construction and reject/retry a step when a numerical invariant fails.
9. **Climatological wind forcing injects momentum.** Latitude-dependent target jets plus prescribed travelling waves are continuously relaxed into u/v. They should not be part of the dynamical equations.

## L0 target architecture

### Horizontal mesh

- 6 gnomonic cubed-sphere faces, 256 x 256 cells/face.
- Precompute in double precision for every cell/edge:
  - center unit vector;
  - cell area;
  - edge great-circle length;
  - outward edge normal in the tangent plane;
  - cell tangent basis;
  - face-neighbour/corner connectivity and vector rotations.
- One shared conservative flux for each physical edge, including cube seams. The two cells on either side consume equal and opposite fluxes.
- Scalars at cell centers; normal wind/momentum on cell edges (C-grid staggering).

### Vertical coordinate

- 30 levels initially, using hybrid sigma-pressure interfaces.
- Prognose surface pressure / column dry mass.
- Layer mass is `Delta p / g`; pressure is not an independent freely-forced field.
- Diagnose hydrostatic geopotential and layer pressure from mass + virtual temperature.
- Terrain-following near the ground, gradually pressure-like aloft.

### Prognostic conserved state

Cell-centered:
- dry-air layer mass;
- potential temperature or dry total energy in mass form;
- water-vapour mass;
- cloud-liquid mass;
- cloud-ice mass;
- optional rain/snow mass when microphysics is reintroduced.

Edge-centered:
- normal horizontal momentum.

Derived, not independently evolved:
- pressure/geopotential;
- divergence;
- vorticity;
- vertical pressure velocity / interface mass flux;
- CAPE/shear diagnostics.

### Horizontal transport

- Flux-form finite volume.
- Same mass flux transports dry mass, heat and every water species.
- Monotone/positivity-preserving reconstruction (start MUSCL/MC; upgrade only after tests pass).
- Geometric Coriolis and pressure-gradient terms use the precomputed spherical metrics.
- Vector fluxes rotate correctly across cube-face seams.

### Time integration

- SSPRK3 for the resolved hydrostatic dynamics.
- Runtime CFL controller; do not assume a fixed 90 s step is safe.
- Start with target horizontal Courant <= 0.45.
- Step rejection/rollback: if CFL, positivity, finite-state or budget tests fail, restore the previous state and retry at half dt.
- Permit dt to grow slowly again after multiple accepted steps.
- Scale-selective divergence damping / fourth-order hyperdiffusion only at the shortest resolved wavelengths. Do not smooth pressure as a primary stabilizer.
- Top sponge only where needed to absorb vertically propagating waves.

## Moisture and surface coupling

All water transfers must close exactly in mass units.

- evaporation: surface reservoir -= dm; lowest atmospheric vapour += dm;
- condensation: vapour -= dm; cloud += dm;
- evaporation of cloud: cloud -= dm; vapour += dm;
- freezing/melting: phase transfer only;
- precipitation fallout: atmospheric hydrometeor -= dm; surface reservoir += dm;
- runoff/ocean exchange must be explicit accounting terms.

No `0..1 precipitation intensity` value is allowed to serve as a mass-transfer variable. Intensity is derived afterward for rendering/UI.

Microphysics is disabled while validating the dry core, then re-enabled one budget-closing process at a time.

## Forcing / climate spin-up

Remove continuous prescribed travelling wind waves from the momentum equations.

Use a staged approach:
1. dry Held-Suarez-like Newtonian temperature relaxation + boundary-layer Rayleigh drag to validate the circulation core;
2. coupled terrain and physically computed surface sensible/latent fluxes;
3. radiative/cloud feedback;
4. moist convection/microphysics;
5. severe-weather parameterizations only after the base moist atmosphere remains bounded for long integrations.

Large-scale climatological forcing must be weak, energy/momentum-accounted and clearly separated from numerical stabilization.

## L1+ non-hydrostatic nests

At ~5 km and below, use a tangent-plane Arakawa C-grid fully compressible non-hydrostatic core with split-explicit integration (large RK step, smaller acoustic/gravity-wave substeps), following the architecture used by WRF/MPAS rather than stretching the hydrostatic L0 equations.

- L1 ~5 km / 48 levels
- L2 ~1 km / 64 levels
- L3 ~250 m / 84 levels
- L4 ~75 m / 112 levels

Parent/child exchange must be conservative. The boundary sponge interpolates parent mass, thermodynamics and momentum; child feedback returns area-integrated conserved tendencies.

## Mandatory validation before physics tuning

### Geometry / horizontal core

1. Uniform constant field remains exactly uniform for 30 simulated days.
2. Solid-body tracer rotation crosses all six cube faces and returns to its start with no seam jump and near-zero mass error.
3. Stationary geostrophic flow remains stationary; no cube-edge or corner source appears.
4. Barotropic/Rossby-Haurwitz test shows no face imprint growing with time.

### Dry 3-D core

5. Hydrostatic resting atmosphere over flat terrain remains at rest.
6. Balanced zonal jet remains balanced without pole/cube-corner artifacts.
7. Jablonowski-Williamson / Polvani baroclinic-wave benchmark reproduces the expected wave growth over ~10-15 days.
8. Held-Suarez dry climate remains statistically bounded for >=1000 simulated days.

### Conservation gates

Every accepted step computes in double precision:
- total dry atmospheric mass;
- total water mass by reservoir;
- total atmospheric + surface water;
- total axial angular momentum;
- total energy for adiabatic tests;
- min/max layer mass, theta, q species;
- max horizontal and vertical Courant numbers;
- NaN/Inf count;
- limiter activation count.

A failed invariant rejects the step; it is never silently repaired with a clamp.

Suggested initial engineering gates (to be tightened after reference tests):
- no negative layer mass or water species;
- no NaN/Inf ever;
- closed-system dry-mass relative drift < 1e-7/day;
- closed-system total-water relative drift < 1e-6/day;
- no persistent 2-delta-x spectral peak;
- no cube-face discontinuity exceeding neighbouring interior truncation error;
- no monotonic growth of domain-mean kinetic energy in an unforced resting test.

## Migration order

1. Build cubed-sphere geometry/connectivity and visualization only.
2. Implement conservative shallow-water test core on it.
3. Pass spherical standard tests.
4. Add 30-level hydrostatic dry atmosphere and adaptive CFL/rollback.
5. Pass dry 3-D benchmarks and long Held-Suarez run.
6. Add surface coupling with exact mass/energy accounting.
7. Add vapour only; pass tracer/water budgets.
8. Add reversible cloud phase changes.
9. Add precipitation with atmosphere-to-surface transfer in kg/m2.
10. Add convection/severe/tropical closures one at a time, each with a budget test.
11. Replace the current L0 backend only after the new core passes the full suite.
12. Rebuild L1+ on the non-hydrostatic C-grid core and then restore adaptive nesting.

## References used for the architecture

- NOAA/GFDL FV3: finite-volume cubed-sphere grid, conservative transport, split-explicit/semi-implicit options, two-way nesting.
- ECMWF IFS/OpenIFS: reduced Gaussian grid, semi-implicit/semi-Lagrangian hydrostatic global core; reduced longitude count towards poles for quasi-uniform physical spacing/CFL.
- NCAR MPAS-Atmosphere: spherical Voronoi C-grid, face-normal velocity, fully compressible non-hydrostatic split-explicit core; Cartesian geometry avoids pole singularity.
- NCAR WRF-ARW: Arakawa C-grid, conservative scalar equations, RK2/RK3 with smaller acoustic/gravity-wave steps.
- Held-Suarez, Polvani, Jablonowski-Williamson and DCMIP idealized dynamical-core benchmarks.
