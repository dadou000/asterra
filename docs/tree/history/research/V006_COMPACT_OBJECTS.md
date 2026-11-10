# V0.0.6 M30 — Compact Object Capability Seam

## Scope

M30 removes the remaining architectural assumption that a celestial body must be
a solid-surface object.

Compact Object and Accretion Flow are ordinary body capabilities. Neither
requires Surface or Reference Shape authority.

The first runtime model is intentionally limited to a replaceable
Schwarzschild baseline. It establishes stable semantic contracts for later Kerr,
ray-integrated lensing and radiative-transfer implementations without pretending
that the baseline is full general relativity.

## Compact Object authority

The Compact Object capability owns:

- gravitational parameter GM in m^3/s^2;
- model selection;
- dimensionless spin and spin axis seam;
- shadow presentation scale;
- lensing-strength presentation control;
- photon-ring presentation intensity.

For the current Schwarzschild baseline, dimensionless spin must be zero.
The spin properties remain semantic so a future Kerr implementation can replace
the runtime model without schema migration.

## Derived Schwarzschild scales

The runtime derives:

- gravitational radius rg = GM / c^2;
- Schwarzschild radius = 2 rg;
- photon sphere = 3 rg;
- non-rotating ISCO = 6 rg;
- critical capture/shadow impact parameter = 3 sqrt(3) rg.

These values are derived runtime products and are never stored as independent
semantic authority.

The weak-field lensing helper uses the asymptotic 4GM/(b c^2) deflection law.
It is not used as a claim of accuracy near the photon sphere; the future optical
solver may replace it behind the same interface.

## Accretion Flow authority

Accretion Flow is independent from Compact Object so alternate flows/emitters
can be attached, removed or replaced without changing gravity/optics authority.

The baseline owns:

- inner/outer radii in gravitational radii;
- flow axis;
- scene-linear emission color and intensity;
- characteristic temperature;
- radial emissivity falloff;
- thickness ratio;
- Doppler presentation strength;
- deterministic seed.

The CPU baseline produces a deterministic radial emission profile. A later GPU
relativistic transfer solver can consume the same semantic record.

## Body registry contract

Universe::BodyShape is now explicitly a conservative spatial/reference envelope,
not surface authority.

For compact bodies, UniverseComposition derives this envelope from the compact
object's optical shadow scale instead of inventing a planet radius. This allows:

- frame/orbit participation;
- culling and representation selection;
- spatial diagnostics;

while the body still has no Surface or Reference Shape capability.

## Validation

M30 regressions currently verify:

- Schwarzschild derived scale relationships;
- deterministic fingerprints;
- weak-field deflection behavior away from the strong-field region;
- deterministic accretion radial profile;
- semantic binding for Compact Object and Accretion Flow;
- compact-body operation with no Surface capability;
- compact-body operation with no Reference Shape capability;
- UniverseComposition deriving its spatial envelope from compact optics;
- semantic edits invalidating the derived envelope/fingerprint.

## Remaining M30 work

- add the first GPU baseline representation: black shadow / photon-ring /
  accretion-flow presentation;
- add Studio diagnostics for rg, Schwarzschild radius, photon sphere, ISCO and
  apparent shadow scale;
- add representation LOD from resolved compact object to far proxy;
- pass integrated Windows/Studio shader/build validation.
