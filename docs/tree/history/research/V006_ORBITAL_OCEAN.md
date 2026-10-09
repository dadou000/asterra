# Orbit V0.0.6 — Orbital Ocean Rendering

Status: M24 implementation baseline.

## Purpose

M24 makes standing planetary water physically recognizable from orbit without creating a second shoreline, sea-level or water-geometry authority.

The accepted V0.0.5 ground representation remains unchanged:

- TerrainSample.elevationMeters is the ground/bed.
- TerrainSample.standingWaterDepthMeters is the canonical exterior standing-water depth.
- Ground terrain renders bed + standing water on the same clipmap triangles.
- The legacy detached OceanRenderer is not reintroduced into the production planetary path.

M24 derives orbital water appearance from that same TerrainSource authority.

## Authority boundary

The Ocean celestial capability contains optical/rendering parameters only.

It does not own:

- coastline;
- wet/dry mask;
- sea level;
- lake geometry;
- water depth;
- terrain geometry.

Those remain owned by terrain/water generation.

The M16 PlanetaryAppearanceProduct continues to sample TerrainSource.

M24 extends each appearance texel with canonical sampled waterDepthMeters in addition to oceanMask.

The ocean mask still derives from standingWaterDepthMeters > the M16 wet threshold.

## Ocean capability

The existing Ocean capability now exposes:

- refractive index;
- orbital roughness;
- RGB absorption coefficient [1/m];
- deep-water linear RGB color;
- sun-glint strength;
- depth used for transition to deep-water color.

These values are editable through the shared Inspector.

The default refractive index is 1.333.

No separate ocean editor is introduced.

## Runtime binding

WorldModel::ResolveOceanBody resolves one enabled Ocean capability into OceanOpticalParameters.

Multiple enabled Ocean capabilities on one body are rejected.

OceanOpticalFingerprint covers every optical parameter and is independent from terrain geometry identity.

Studio therefore tracks separately:

- macro-globe geometry fingerprint;
- base terrain appearance fingerprint;
- ocean optical fingerprint;
- cloud fingerprint;
- final composed appearance fingerprint.

## Shared optical model

Orbit::CelestialOcean provides the CPU/reference optical model.

### Normal-incidence dielectric reflectance

For outside and inside indices n1 and n2:

```
F0 = ((n1 - n2) / (n1 + n2))^2
```

For n1 = 1 and water n2 = 1.333, F0 is approximately 0.0204.

### Fresnel

Schlick Fresnel is used for the angular response:

```
F = F0 + (1 - F0) (1 - VdotH)^5
```

This gives the expected strong grazing reflection without the old fixed limb-highlight heuristic.

### Microfacet sun glint

Orbital glint uses a GGX-style microfacet BRDF:

- GGX/Trowbridge-Reitz normal distribution;
- Smith-style geometric visibility;
- dielectric Fresnel;
- authorable ocean roughness;
- authorable glint strength.

The highlight depends on the actual body-space light and camera/view directions.

Therefore the glint moves correctly as the observer or star direction changes.

## Deep-water color and absorption

Water-column transmittance uses Beer-Lambert attenuation:

```
T_rgb(d) = exp(-absorption_rgb * d)
```

ApplyOrbitalOceanAppearance reads canonical M16 waterDepthMeters.

Ocean texels transition from the existing shoreline/shallow appearance toward the authored deep-water color according to actual sampled depth.

The same pass sets ocean roughness from the Ocean capability.

It never modifies:

- oceanMask;
- waterDepthMeters;
- terrain geometry.

The ocean optical fingerprint is combined into the final appearance fingerprint.

## Composition order

The permanent orbital appearance order is:

```
Terrain / climate / water authority
    -> M16 base appearance
    -> M24 ocean optical transform
    -> M23 cloud composition
    -> macro globe / far representation
```

This ordering matters:

- ocean color derives from water depth;
- clouds visually cover the already resolved land/ocean surface;
- clouds can attenuate ocean direct light/glint.

## M23 cloud integration

PlanetaryAppearance now contains a derived directLightTransmittance channel.

Base M16 output initializes it to 1.

M23 cloud composition multiplies it by cloud optical transmission.

Macro-globe vertices pack it into the fourth material channel.

Far-body AppearanceSummary carries the mean transmission.

M24 multiplies direct ocean glint by this transmission.

Thus a thick cloud deck cannot leave an unattenuated sun glint beneath it.

This channel is derived presentation data and does not become weather authority.

## Macro globe

Macro globe rendering uses:

- per-texel oceanMask;
- per-texel cloud direct-light transmission;
- per-texel ice mask;
- live camera direction;
- live M20 source direction;
- M19/M20 incident-light scale;
- Ocean refractive index;
- Ocean roughness;
- Ocean glint strength.

Both the color-only path and surface-output path use the same ocean BRDF.

Ocean glint is suppressed by ice.

## Smooth globe and analytic disc

FarBodyRenderer now carries the same ocean optical controls.

Smooth globe and analytic-disc representations evaluate live Fresnel/GGX glint.

The average M16/M23 appearance supplies:

- ocean fraction;
- ice fraction;
- direct-light transmission;
- resolved water color/roughness.

This preserves a coherent ocean identity as the representation ladder moves away from the macro globe.

## Cached disc behavior

A cached disc is precomputed appearance and cannot correctly preserve a moving view-dependent sun glint.

Therefore when all of these are true:

- Ocean capability is enabled;
- ocean fraction is non-zero;
- live incident light is non-zero;

FarBodyRenderer bypasses the cached texture and renders the analytic disc path using live ocean lighting.

The cached disc remains valid when no live ocean glint is required.

## M19 / M20 illumination

Studio already resolves the brightest valid M19 radiative source through M20.

The resulting direction and irradiance scale feed the M24 BRDF.

Celestial eclipses therefore reduce ocean diffuse and specular lighting through the same M20 irradiance path.

No ocean-specific star/light calculation exists.

## Ground renderer relationship

M24 intentionally does not revive engine/water_render/OceanRenderer for planetary ground rendering.

That renderer uses an independent spherical mesh and the project has already documented why combining independent ocean/terrain meshes causes chord/LOD intersection artifacts.

Production terrain remains the same connected wet/dry heightfield surface.

Future near-water refraction, underwater rendering, waves and fluid-volume effects should extend that ground path or consume WaterService state without changing M24's orbital authority contract.

## Diagnostics

StudioOceanDiagnostics exposes:

- body;
- ocean optical fingerprint;
- refractive index;
- orbital roughness;
- glint strength;
- resolved orbital ocean fraction.

This is diagnostic/view state only.

## Validation

M24 deterministic coverage includes:

- dielectric water F0 near 0.0204;
- stronger Fresnel reflectance at grazing angles;
- aligned GGX glint exceeding off-specular response;
- depth-dependent Beer-Lambert absorption;
- deeper water converging toward the deep-water color;
- ocean optical fingerprint generation;
- water-depth appearance transform changing final appearance fingerprint;
- preservation of canonical oceanMask;
- preservation of M23 directLightTransmittance through ocean processing;
- semantic Ocean capability resolution;
- optical-property revision changing the ocean fingerprint.

## Intentional limits

M24 does not yet implement:

- orbital wave geometry;
- whitecaps/foam field;
- dynamic wind-wave spectrum;
- polarization;
- spectral water optics beyond RGB coefficients;
- underwater rendering;
- refraction through a separate water volume;
- ocean-current simulation;
- tides.

Those features can extend WaterService / ground rendering / derived ocean appearance later without changing coastline or sea-level authority.
