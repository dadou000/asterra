# Orbit V0.0.6 — Gas and Ice Giant Appearance

Status: M27 implementation baseline.

## Purpose

M27 adds a dedicated visual representation for gas and ice giants without inventing terrain or a solid-surface authority.

A giant body remains an ordinary Celestial Body with ordinary shape, mass, orbit, rotation, gravity and optional atmosphere capabilities.

The new Giant Appearance capability owns only procedural cloud-top appearance parameters.

## Authority boundaries

Giant Appearance does not own:

- body shape;
- mass/gravity;
- rotation/orbit;
- atmospheric density/scattering;
- radiative emission;
- ring systems;
- a terrain surface.

M21 Atmosphere remains the physical atmosphere/scattering authority when present.

M19/M20 remain the radiative/direct-light authority.

Giant Appearance is therefore a visual cloud-top/material authority used by the existing representation ladder.

## Semantic capability

Giant Appearance is a singleton celestial capability.

It exposes:

- Giant Class: Gas Giant / Ice Giant;
- deterministic seed;
- base cloud color;
- band cloud color;
- polar color;
- band frequency;
- band strength;
- zonal shear;
- storm strength;
- storm scale;
- polar variation;
- cloud-depth contrast;
- turbulence strength;
- turbulence scale.

Advanced controls remain in the shared Properties Inspector.

No separate giant editor is introduced.

## Gas and ice class defaults

Gas Giant baseline:

- warm neutral cloud-top palette;
- stronger zonal band contrast;
- stronger storm activity;
- moderate polar variation.

Ice Giant baseline:

- blue/cyan methane-like palette;
- lower band contrast;
- lower storm activity;
- stronger smooth polar variation.

The class affects fallback values only.

Explicitly authored colors and controls remain authoritative.

## CPU appearance product

Orbit::CelestialGiants builds a deterministic cube-sphere PlanetaryAppearanceProduct.

This reuses the same derived appearance contract already consumed by far rendering and GPU presentation caches.

The product is not persistent authority.

Its fingerprint includes every semantic giant parameter and the appearance-product resolution.

## Zonal band model

Cloud bands are latitude-driven.

The baseline combines two sinusoidal latitude frequencies and longitudinal perturbation.

A zonal-shear term warps the bands using broad and fine deterministic turbulence.

This creates alternating belts/zones without treating a baked texture as authority.

## Turbulence and depth variation

Two deterministic multi-octave body-space fields are evaluated:

- broad atmospheric structure;
- finer turbulence.

The fields affect:

- band shear;
- local cloud-depth contrast;
- fine cloud brightness.

Depth contrast modulates cloud-top brightness rather than introducing fake terrain displacement.

## Storms / vortices

The CPU reference path places deterministic seeded oval storm regions in body space.

Storm longitude/latitude and characteristic radius derive from the semantic seed.

The live shader uses a deterministic low-frequency storm field from the same seed/strength/scale family.

The exact CPU and live procedural implementations are both derived products of the same semantic parameters; neither is authority.

## Polar variation

A latitude-dependent polar mask blends toward the authored polar color.

Gas and ice giants use different baseline exponents/strengths through their class-specific fallback parameters.

## Groundless representation ladder

Giant bodies do not require Surface/Terrain Authority.

Studio resolves them through the existing groundless M14 ladder:

- Smooth Globe;
- Analytic Disc;
- Point Proxy.

No production-terrain or macro-displaced terrain representation is introduced.

## Live far rendering

FarBodyRenderer reuses the existing fixed 40-dword appearance push-constant layout.

Stellar and giant visual payloads are mutually exclusive.

For giant smooth/disc rendering, existing appearance-only channels carry:

- base color;
- band color;
- polar color;
- band frequency/strength;
- seed;
- zonal shear;
- storm strength/scale;
- depth contrast;
- turbulence strength.

The shader reconstructs banding and storms directly from body-space normal.

This keeps terminators and illumination live under M20 rather than baking lighting into an impostor.

## Point proxies

When the body is unresolved, the renderer uses the average color/roughness derived from the deterministic CPU giant appearance product.

This provides continuous visual identity across the smooth/disc/point ladder.

## GPU derived product

Studio retains a disposable GpuPlanetaryAppearanceProduct for each active giant presentation.

It is rebuilt only when the Giant Appearance semantic fingerprint changes.

The GPU buffer is presentation/cache state, not semantic authority.

## Studio diagnostics

StudioGiantDiagnostics exposes:

- body;
- appearance fingerprint;
- gas/ice class;
- band frequency;
- band strength;
- storm strength;
- projected radius;
- active M14 representation.

This is intended for M32 diagnostics/quality tooling.

## Lighting integration

Giant smooth/disc rendering uses the ordinary M20 body-fixed direct-light direction and irradiance scale.

There is no giant-specific star/light calculation.

M21 Atmosphere may coexist on the same body and remains responsible for physical scattering/LUT products.

## G-buffer behavior

The procedural giant payload is visual-only.

Far surface/G-buffer outputs use the CPU-derived average giant albedo and roughness.

This prevents zonal-shear/storm parameter packing from being misinterpreted as physical roughness/emission data by the shared lighting/GI path.

## Validation

Deterministic coverage includes:

- same parameters/seed produce the same appearance fingerprint;
- cube-sphere output contains spatial color variation;
- gas/ice class changes fingerprint;
- equatorial and polar appearance differ;
- semantic Ice Giant resolution uses class-aware fallback colors;
- giant bodies operate without a Surface capability;
- semantic edits invalidate the giant fingerprint;
- GPU appearance state is separate from semantic authority.

## Intentional limits

M27 does not yet implement:

- physical convective fluid simulation;
- time-evolving Navier-Stokes zonal jets;
- deep atmospheric volumetric ray marching;
- lightning within giant storms;
- spectral methane/ammonia absorption;
- true multilayer cloud pressure surfaces;
- dynamic storm birth/decay.

Those can extend the same semantic parameters or add explicit simulation capabilities later without creating a baked-texture authority.
