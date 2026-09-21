# Orbit V0.0.6 — Eclipse, Transit and Reflected-Light Service

Status: M20 implementation baseline.

## Purpose

M20 adds render-LOD-independent celestial lighting geometry on top of the M19 radiometric emitter model.

The service operates on authoritative body positions/radii and semantic radiative emitters. It does not depend on macro-globe, impostor or point-proxy choice.

## Finite-disc occultation

For an observer/receiver, source and occluder are treated as finite apparent discs.

Angular radius is:

```
alpha = asin(radius / distance)
```

Angular separation is derived from normalized source/occluder directions.

An occluder only contributes when its center is closer to the receiver than the source center.

Single-occluder overlap uses the analytic circle-intersection area of the apparent discs.

Results expose:

- source angular radius;
- occluder angular radius;
- angular separation;
- obscured fraction;
- visible fraction;
- front/overlap state;
- total-eclipse state;
- annular-eclipse state.

## Multiple occluders

Independent visibility fractions are not multiplied.

For more than one contributing occluder, M20 evaluates the union of projected occulting discs over the source disc with a deterministic equal-solid-angle golden-angle sample set.

This prevents overlapping moons from being counted twice.

The one-occluder path remains analytic.

The current source disc assumes uniform radiance for occultation area. Limb darkening can later weight the same sample seam without changing body geometry or authority.

## Direct irradiance

The M19 source luminosity is converted to unoccluded irradiance with:

```
E = L / (4*pi*d^2)
```

The finite-disc visibility fraction attenuates that irradiance:

```
E_visible = E * visibleFraction
```

## FrameGraph runtime service

WorldModel::CelestialLightingService resolves:

- receiver body;
- radiative emitter body;
- finite occluder bodies;
- body-center positions through FrameGraph at arbitrary SimulationTime;
- photosphere radius from the M19 semantic radiative binding;
- body-fixed source direction for renderer use.

No render representation is queried.

## Reflected light

M20 provides a Lambert-sphere baseline.

The Lambert phase function is:

```
Phi(alpha) =
    [sin(alpha) + (pi-alpha) cos(alpha)] / pi
```

The service exposes unit-geometric-albedo observer irradiance:

```
E_observer(A_g=1) =
    E_incident *
    (R / d_observer)^2 *
    Phi(alpha)
```

Material/surface authority can multiply this by geometric albedo later.

This keeps orbital geometry/radiometry separate from render LOD and material authoring.

## Studio rendering integration

For each targeted body, Studio:

1. enumerates enabled radiative bodies in the same Celestial System;
2. evaluates each source with every other body available as a finite occluder;
3. selects the surviving source with the greatest direct irradiance;
4. converts its body-fixed direction into the renderer light input;
5. normalizes incident irradiance against the M19 1361 W/m^2 calibration reference.

Macro-globe, smooth-globe and analytic-disc terminators now follow this live source direction and eclipse attenuation.

Dynamic celestial lighting selects the analytic disc path instead of the M18 cached disc, because the current cached product contains static canonical lighting.

BodyMap uses the same live direct-light state.

Non-terrain reflective far bodies also receive the same direct-light state.

## Unresolved reflected bodies

Point proxies use the M20 Lambert phase function from the actual body-to-emitter and body-to-camera directions.

Their reflected intensity combines:

- incident-light scale;
- Lambert phase;
- existing M18 projected-area/sub-pixel correction.

Self-luminous stellar point proxies remain driven by M19 radiometry instead.

## Diagnostics

StudioCelestialLightingDiagnostics exposes per viewport:

- receiver BodyId;
- selected emitter BodyId;
- visible source fraction;
- direct irradiance [W/m^2];
- contributing occluder count.

This is the M20 seam for the later M32 diagnostics UI.

## Validation

Deterministic tests cover:

- no-overlap visibility;
- total eclipse;
- annular eclipse;
- behind-source rejection;
- direct irradiance attenuation;
- multi-occluder union behavior;
- duplicate/overlapping occluders not being double-counted;
- Lambert phase endpoints;
- reflected-light observer irradiance;
- end-to-end FrameGraph star/moon/planet eclipse composition;
- reflected-light propagation through semantic bodies.

## Intentional limits

M20 does not yet implement:

- spectral wavelength-dependent eclipses;
- stellar limb-darkening weighted occultation;
- atmospheric refraction through an eclipsing body;
- shadow-volume rasterization on a planet surface;
- multiple simultaneous render-light accumulation;
- mutual illumination between many reflective bodies.

The CPU service is already emitter-agnostic and can evaluate all sources independently. Studio currently renders the brightest surviving source as the baseline direct light. Multi-source render accumulation can be layered on this service later without changing semantic or orbital authority.
