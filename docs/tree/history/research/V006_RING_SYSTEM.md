# Orbit V0.0.6 — Ring System

Status: M25 implementation baseline.

## Purpose

M25 introduces one semantic ring-system authority with arbitrary authored radial bands and disposable near/far render products.

Ring geometry is never persisted as celestial authority.

The persistent hierarchy is:

```
Celestial Body
  -> Ring System
       -> Ring Band
       -> Ring Band
       -> ...
```

There is no authoring band-count cap.

## Semantic authority

### Ring System

The Ring System capability owns:

- body-frame ring-plane normal;
- whether the rings cast shadows onto the body;
- whether ring particles receive the host body's shadow.

The supported baseline model is:

```
Particle Distribution
```

### Ring Band

Each Ring Band owns one physical radial interval:

- inner radius [m];
- outer radius [m];
- normal optical depth;
- single-scattering albedo;
- phase anisotropy;
- linear RGB particle color;
- physical thickness [m].

The supported baseline model is:

```
Physical Band
```

Bands are ordinary semantic children of Ring System and are inspectable/editable with the shared Explorer + Properties Inspector.

## Authoring workflow

The unified Celestial panel exposes Add Ring Band whenever a Ring System is selected.

Ring bands are not body-level capabilities.

They can be removed independently.

A Ring System that still owns Ring Band children cannot be accidentally deleted by the ordinary capability-removal action.

No separate ring editor is introduced.

## Runtime binding

WorldModel::ResolveRingSystem:

1. finds the single enabled Ring System on a body;
2. resolves all enabled Ring Band children;
3. sorts them deterministically by physical inner radius, then stable semantic ObjectId;
4. validates the complete system;
5. emits one celestial_rings::RingSystem runtime record;
6. computes a deterministic semantic fingerprint.

Multiple enabled Ring Systems on one body are rejected.

## Validation

The current physical baseline requires:

- finite non-zero ring-plane normal;
- outer radius > inner radius;
- nonnegative optical depth;
- single-scattering albedo in [0,1];
- anisotropy in (-1,1);
- nonnegative color channels;
- nonnegative physical thickness;
- non-overlapping radial bands.

Gaps between bands are allowed.

Overlapping bands are rejected rather than silently composited because overlapping semantic intervals would create ambiguous optical authority.

## Optical-depth model

Ring Band optical depth is defined normal to the ring plane.

For a ray with ring-plane cosine:

```
mu = abs(dot(ray, ringNormal))
```

slant-path transmission is:

```
T = exp(-tau / max(mu, epsilon))
```

This same normal optical-depth convention is used for:

- observer opacity;
- incident stellar interception;
- shadows cast by rings onto the body.

## Particle scattering

M25 uses a Henyey-Greenstein phase function as a compact single-scattering baseline:

```
P(cos(theta), g) =
    (1 - g^2) /
    (4 pi (1 + g^2 - 2 g cos(theta))^(3/2))
```

This is intentionally a parameterized phase approximation, not a claim that real planetary rings are exactly HG scatterers.

The semantic anisotropy control can later be replaced or supplemented by measured/tabulated phase functions without changing ring geometry authority.

## Ring shadows on the body

RingShadowTransmittanceAtSurface evaluates an analytic ray-plane intersection.

Given:

- body-surface point;
- direction to the active M20 emitter;
- ring plane;

the ray is intersected with the ring plane.

The planar intersection radius selects the semantic Ring Band.

If the ray passes through a band:

```
surfaceIrradiance *=
    exp(-tau / abs(dot(lightDirection, ringNormal)))
```

If the intersection is inside a ring gap or misses the forward ray, transmission remains 1.

## Shared M20 / M23 / M25 surface lighting

CelestialLightingService exposes three surface-light paths:

- clouds only;
- rings only;
- clouds + rings.

The combined ordering is conceptually:

```
M20 finite celestial-disc visibility
    -> M23 cloud transmission
    -> M25 ring transmission
```

The returned DirectSurfaceLighting keeps separate:

- celestial.visibleFraction;
- cloudTransmittance;
- ringTransmittance;
- final irradiance.

Ring shadows therefore do not modify M20 eclipse/transit classification.

Cloudless ringed bodies do not require a fake cloud product.

## Body shadow on rings

BodyShadowTransmittanceAtRingPoint performs an analytic ray/sphere intersection from a ring particle toward the active emitter.

If the host body lies between the particle and the star, direct illumination is zero for that ring point.

The live GPU ring renderer evaluates the same concept per fragment.

### Shape limitation

The M25 baseline uses the host body's reference radius for body/ring shadow intersection.

This is exact for spherical hosts and an approximation for oblate/irregular hosts.

A future ellipsoid/irregular shadow solver can replace the intersection function without changing Ring System or Ring Band authority.

M28 is the natural integration point for irregular-body refinement.

## Near derived representation

BuildRingMesh creates a planar annulus mesh for every enabled band.

The mesh contains:

- normalized body-space position;
- particle linear color;
- normal optical depth;
- single-scattering albedo;
- anisotropy.

Near Studio presentation currently uses 256 angular segments per band.

This number is a derived presentation policy, not an authoring limit.

## Far derived representation

M25 has two lower-cost derived products:

- a 64-segment far ring mesh used by Studio rendering;
- FarRingProfile, a radial optical profile containing sampled radius, optical depth, color, albedo and anisotropy.

The far radial profile is intended as the stable input seam for later analytic/disc-scale ring impostors and cache budgeting.

Current Studio profile resolution is 256 samples.

Derived product fingerprints include:

- semantic Ring System fingerprint;
- body reference scale;
- derived mesh/profile resolution.

## LOD selection

Studio computes the projected angular radius of the outermost ring.

Current baseline selection is:

- projected outer radius >= 180 px: near mesh;
- smaller: far mesh.

The selection depends on camera distance, vertical field of view and viewport height.

No semantic data changes during LOD switching.

M31 can move this threshold into the centralized celestial quality/performance scheduler.

## Live ring renderer

RingRenderer is an RGBA16F alpha pass executed after opaque celestial surface lighting.

The shader evaluates:

- view-path optical depth;
- incident stellar optical-depth interception;
- single-scattering albedo;
- HG phase response;
- live M20 light direction;
- live M20 irradiance scale;
- analytic host-body shadow on ring particles;
- analytic host-body occlusion of back-side ring fragments.

Back-side ring fragments hidden by the body are discarded analytically, so the current celestial surface paths do not need a separate shared depth representation just for rings.

## Studio integration

Studio maintains one RingPresentation per viewport/body:

- semantic fingerprint;
- reference radius;
- near GPU mesh;
- far GPU mesh;
- far radial profile.

Products rebuild only when semantic ring authority or reference scale changes.

Camera movement only changes LOD selection and live shading.

StudioRingDiagnostics exposes:

- body;
- semantic ring fingerprint;
- band count;
- inner/outer physical radii;
- projected outer radius;
- near/far selection;
- angular segment count;
- far-profile sample count.

Rings are deliberately omitted from exclusive terrain-debug mode.

## M19 / M20 integration

Rings do not discover or invent a separate light source.

Studio uses the already selected M19/M20 direct emitter:

- body-fixed direction to emitter;
- eclipse-attenuated irradiance scale.

Planetary eclipses therefore attenuate ring illumination naturally.

Ring particle shadowing by the host body is evaluated afterward at the ring point.

## Deterministic validation

Regression coverage includes:

- semantic ring fingerprint generation;
- increasing Fresnel-independent slant optical attenuation;
- anisotropic forward/back phase response;
- deterministic near-mesh topology;
- far radial-profile generation;
- analytic ring shadow crossing a body-surface ray;
- analytic body shadow on a ring particle;
- semantic Ring Band sorting independent of creation order;
- Ring Band optical edit invalidating the semantic fingerprint;
- FrameGraph-resolved star/planet lighting;
- ring transmission reducing final surface irradiance;
- M20 stellar visible fraction remaining unchanged by ring attenuation;
- ring-only lighting leaving cloud transmission at 1.

## Intentional limits

M25 does not yet implement:

- particle-resolved ring dynamics;
- self-gravity wakes;
- resonant density waves;
- shepherd-moon procedural structures;
- multiple-scattering radiative transfer;
- spectral particle phase tables;
- polarization;
- ellipsoidal/irregular host shadow intersection;
- ring thermal emission;
- time-varying radial band simulation.

Those systems can extend the Ring Band optical/dynamic model without making tessellation persistent authority.
