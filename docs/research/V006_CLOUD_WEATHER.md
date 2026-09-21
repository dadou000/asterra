# Orbit V0.0.6 — Cloud Capability and Orbital Weather Representation

Status: M23 implementation baseline.

## Purpose

M23 introduces one cloud/weather authority that is shared by:

- climate-driven cloud placement;
- procedural cloud placement;
- authored/imported coverage adapters;
- orbital cloud appearance;
- surface direct-light shadowing;
- later close-range volumetric/froxel rendering.

Clouds remain a separate capability from the M21 Atmosphere authority.

## Research basis

The architecture follows the established real-time volumetric-cloud pattern of separating:

- a low-frequency weather/coverage field;
- high-frequency volumetric/detail structure;
- lighting/shadow evaluation.

The Nubis/Horizon cloud work is the principal reference for this separation because its weather-map representation carries coverage/type/precipitation authority independently from the expensive volume representation.

M23 implements Orbit's persistent low-frequency authority and derived orbital/shadow products. It intentionally does not make a dense voxel volume the persistent weather source.

## Semantic cloud layers

Cloud Layer remains an ordinary repeatable celestial capability.

Each enabled layer exposes:

- source model;
- base altitude;
- top altitude;
- coverage bias;
- peak optical depth;
- single-scattering albedo;
- phase anisotropy;
- density exponent;
- weather scale;
- detail scale;
- deterministic seed;
- angular wind velocity;
- precipitation phase metadata;
- cloud-shadow participation;
- orbital visibility;
- optional Source Object.

Multiple cloud layers may coexist on one body.

Stable semantic ObjectId halves are carried into derived layer fingerprints.

## Source authority

CloudSourceModel supports:

- Climate Procedural;
- Procedural;
- Authored;
- Imported.

### Climate Procedural

Uses the existing TerrainSource climate authority.

TerrainSample already provides:

- temperature;
- humidity;
- precipitation;
- continentality.

The current M23 coverage baseline uses humidity and precipitation directly, so existing rain shadows and climate revisioning automatically affect cloud fields.

No second climate database is created.

### Procedural

Uses deterministic body-space weather/detail functions driven by:

- seed;
- weather scale;
- detail scale;
- coverage bias.

This path requires no terrain source.

### Authored / Imported

M23 defines CloudCoverageSource:

```
SampleCoverage(unitDirection, SimulationTime)
Revision()
```

An authored or imported Source Object is therefore expected to expose an adapter implementing this contract.

The external source revision participates in the cloud-field fingerprint.

Studio deliberately does not silently substitute procedural weather when an authored/imported adapter is missing.

## Shared cube-sphere weather field

CloudFieldProduct is a derived six-face body-space field.

Each layer stores per texel:

- coverage;
- optical depth;
- single-scattering albedo;
- anisotropy.

The product stores:

- face resolution;
- sample footprint;
- climate revision;
- simulation-time bucket;
- total fingerprint;
- stable layer products.

The same field is sampled by both orbital appearance and cloud-shadow evaluation.

## Temporal evolution

Layer wind is expressed as body-frame angular velocity [rad/s].

Sampling advects the weather coordinate by the inverse angular rotation at the requested simulation time.

The field fingerprint includes a configurable simulation-time quantum.

This prevents frame-rate-dependent weather identity while still allowing deterministic weather animation.

## Fingerprints

Cloud identity includes:

- reference radius;
- field resolution;
- footprint scale;
- time bucket;
- climate revision when used;
- authored/imported coverage revision when used;
- every layer semantic ID;
- source mode;
- geometric layer heights;
- optical/scattering controls;
- noise controls;
- wind;
- shadow/orbital participation.

Derived GPU products are disposable and are never persisted as weather authority.

## Orbital weather representation

GpuCloudFieldProduct packs the complete cube-sphere weather field into a structured GPU buffer.

Studio caches one CPU + GPU cloud product per viewport/body and rebuilds only when its cloud fingerprint changes.

Diagnostics expose:

- body;
- cloud fingerprint;
- climate revision;
- time bucket;
- layer count;
- mean coverage;
- mean optical depth;
- GPU residency.

## Orbital visual integration

For orbital globe/cached-disc rendering, M23 composites orbital-visible cloud layers into the M16 PlanetaryAppearanceProduct.

The composition:

- derives opacity from cloud coverage and optical depth;
- uses layer single-scattering albedo for bright cloud reflectance;
- raises apparent roughness for cloud-covered texels;
- combines the cloud fingerprint into the appearance fingerprint.

The macro-globe cache tracks independently:

- base terrain appearance fingerprint;
- cloud fingerprint;
- final composed appearance fingerprint.

Therefore terrain/climate/cloud changes invalidate only the appropriate derived products.

Layers with Visible From Orbit disabled are skipped by orbital appearance composition.

## Ground/orbit consistency

Ground rendering does not own another cloud map.

The production terrain/weather path and orbital path sample the same CloudFieldProduct authority.

Future close-range volume/froxel rendering should refine the field spatially rather than regenerate different large-scale coverage.

## Cloud shadows

CloudShadowTransmittanceAtSurface evaluates each shadow-participating layer along the body-fixed light direction.

The surface point is projected to the layer's mean spherical shell.

Sampled optical depth is corrected by incidence angle and converted to transmission with Beer-Lambert attenuation:

```
T_cloud = exp(-tau / mu)
```

Multiple participating layers multiply transmission.

## M20 lighting integration

CelestialLightingService now provides DirectLightingAtSurface.

The service first evaluates the ordinary M20 direct source:

- emitter luminosity;
- distance;
- finite-disc celestial eclipses.

It then evaluates M23 cloud transmission at the requested receiver surface direction.

The returned irradiance is:

```
E_surface =
    E_M20 *
    T_cloud
```

This preserves M20 celestial visibility independently from cloud transmission.

Terrain, ocean, vegetation and later atmospheric/froxel consumers can therefore use one direct-light result.

## Recipes

Atmosphere-bearing rocky-planet recipes now also create an ordinary Climate Procedural Cloud Layer.

Seed, coverage bias and optical depth are deterministic recipe outputs.

The resulting cloud capability remains fully editable and uses the normal semantic/Inspector path.

## Validation

Regression coverage includes:

- deterministic climate-derived cloud field generation;
- climate revision participation;
- simulation-time fingerprint changes;
- wind-driven evolution;
- cloud-shadow participation disable;
- semantic multi-layer resolution;
- procedural layer generation without terrain climate;
- orbital appearance fingerprint/composition;
- surface irradiance reduction through CelestialLightingService;
- preservation of M20 celestial visible fraction while clouds attenuate irradiance.

## Intentional limits

M23 does not yet implement the final close-range dense volumetric renderer.

In particular:

- no persistent voxel weather authority;
- no cloud microphysics simulation;
- no convective cell solver;
- no lightning volume lighting;
- no precipitation particle system;
- no cloud self-shadow volume cache;
- no volumetric temporal reconstruction.

Those systems should consume/refine the M23 field rather than create competing large-scale cloud coverage.

The GPU field and orbital composition seam are intentionally compatible with later Nubis-style 3D/froxel/voxel detail systems.
