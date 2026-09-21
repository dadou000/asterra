# Orbit V0.0.6 — Radiometric Celestial Emitters and HDR Exposure

Status: M19 implementation baseline.

## Purpose

M19 replaces arbitrary stellar brightness with explicit SI radiometry and introduces a shared HDR/display boundary.

RadiativeEmitter and Photosphere remain ordinary semantic capabilities.

## Semantic authoring

RadiativeEmitter now exposes:

- bolometric luminosity [W];
- effective temperature [K];
- emissivity [0..1];
- derive-luminosity-from-photosphere toggle.

Photosphere now exposes:

- photosphere radius [m];
- effective temperature [K].

Star recipes populate these fields explicitly.

## Physical model

For blackbody/graybody emitters:

```
M = emissivity * sigma * T^4
L = 4*pi*R^2*M
radiance = M/pi
```

with Stefan-Boltzmann constant:

```
sigma = 5.670374419e-8 W m^-2 K^-4
```

For unresolved emitters:

```
E = L / (4*pi*d^2)
```

For resolved photosphere pixels:

```
pixelIrradiance = surfaceRadiance * pixelSolidAngle
```

where central pixel solid angle is derived from vertical FOV and viewport height.

## Semantic runtime binding

WorldModel::ResolveRadiativeBody reads the enabled RadiativeEmitter and optional Photosphere beneath a Celestial Body and resolves a reusable SI RadiativeState.

It rejects duplicate enabled emitters/photospheres and unsupported models.

This is the M20 reflected-light source seam.

## Exposure calibration

ExposureSettings defines:

- reference irradiance;
- middle gray;
- compensation stops.

Default calibration maps 1361 W/m^2 to 0.18 scene-linear at zero compensation.

No hidden stellar display multiplier is used.

## Resolved vs unresolved continuity

Resolved smooth/disc photospheres receive exposed per-pixel irradiance from conserved surface radiance.

Point/stellar-point proxies receive exposed integrated inverse-square irradiance.

The M18 sub-pixel projected-area correction remains active.

This preserves energy behavior across disc -> point representation changes.

## HDR boundary

RenderView now owns two color targets:

- scene color: RGBA16F scene-linear HDR;
- display color: RGBA8 display-referred.

Renderers write scene color.

Studio UI samples DisplayColor only.

The existing final LUT pass now performs:

1. exposure scale;
2. Reinhard tone mapping;
3. display LUT correction.

Celestial globe/far paths touched by M15-M19 now leave tone mapping to this shared display resolve.

## Validation

Deterministic coverage includes:

- solar luminosity from R/T within 1%;
- solar irradiance near 1 AU;
- exposure reference -> middle gray;
- exposure-stop doubling;
- pixel-solid-angle radiance bridge;
- tone-map reference values;
- generated-star semantic radiometry binding.

## Intentional limits

M19 does not yet implement:

- spectral blackbody RGB;
- physically based reflected planetary lighting;
- eclipse/transit attenuation;
- atmospheric eye adaptation;
- automatic exposure adaptation;
- bloom/star diffraction.

Those belong to M20, M21, M26 and later quality/performance work.
