# Orbit V0.0.6 — Stellar Rendering

Status: M26 implementation baseline.

## Purpose

M26 adds full stellar appearance above the existing M19 radiometric authority.

The critical separation is:

- M19 owns luminosity, radius, effective temperature, emissivity, irradiance and scene-radiometric scaling.
- M26 owns only how a radiative photosphere is visually represented.

Screen-space corona, glare and diffraction never become light sources and never feed back into the M19 luminosity solve.

## Semantic authority

The existing Photosphere capability remains the only stellar surface authoring object.

M26 adds editable appearance controls:

- limb darkening;
- granulation strength;
- granulation scale;
- activity level;
- deterministic activity seed;
- chromosphere strength and extent;
- corona strength and extent;
- glare strength;
- glare radius in pixels.

The existing Photosphere radius and effective temperature remain physical/radiometric properties shared with M19.

No new Star subclass or separate stellar editor is introduced.

## Runtime binding

WorldModel::ResolveRadiativeBody now returns both:

- the unchanged M19 RadiativeState;
- M26 StellarAppearanceParameters;
- temperature-derived stellar linear RGB;
- a dedicated stellar appearance fingerprint.

Changing appearance-only fields changes the M26 fingerprint without changing:

- luminosity;
- photosphere radius;
- effective temperature;
- emissivity.

Changing effective temperature updates both M19 radiometry and the M26 temperature color because temperature is shared physical authority rather than duplicated display state.

## Temperature color

M26 derives stellar chromaticity from a Planckian-locus CIE xy approximation in the supported approximation range and converts XYZ to linear RGB.

Only the chromaticity approximation is clamped to its fitted range.

The physical M19 temperature itself is not clamped or rewritten.

The derived RGB is normalized as an appearance chromaticity; absolute brightness still comes from M19 radiance/irradiance.

## Limb darkening

The CPU reference and GPU resolved-disc path use the linear law:

I(mu) / I(1) = 1 - u (1 - mu)

where:

- mu is the cosine of the emission angle;
- u is the authorable limb-darkening coefficient.

This replaces the previous hard-coded 0.58 + 0.42 mu stellar disc response.

## Granulation

Granulation is deterministic body-surface modulation.

The reference path uses a seeded multi-octave body-direction field.

The GPU path evaluates the same architectural idea directly from the visible surface direction.

Granulation changes local apparent radiance only.

It does not modify the M19 integrated luminosity authority.

## Activity

Activity is a deterministic large-scale field controlled by:

- Activity Level;
- Activity Seed.

High field values create dark spot modulation.

Near-threshold values create a smaller facular brightening term.

The implementation is intentionally a visual baseline rather than a stellar dynamo simulation.

Future activity-cycle or magnetic simulation should drive these appearance controls rather than create a competing photosphere authority.

## Chromosphere

The chromosphere is rendered immediately outside normalized photospheric radius 1.

Its radial profile is an exponential falloff over the authorable extent.

It is warm/red biased as a presentation layer and remains in HDR scene color.

The chromosphere is not written to the physical photosphere emission surface buffer.

## Corona

The corona is an off-limb HDR appearance profile with an inverse-power radial falloff over the authorable extent.

Its color is biased toward a pale blue-white mix while remaining tied to the temperature-derived stellar color.

The corona does not alter M19 irradiance.

## Resolved representation

Stellar Smooth Globe and Analytic Disc representations share the M26 stellar disc shader.

For stars, Smooth Globe presentation intentionally uses the analytic photospheric disc path because a spherical radiative photosphere does not require planetary surface-light BRDF evaluation, and the screen-space path can extend the chromosphere/corona beyond the physical limb.

Resolved stellar color is:

temperature chromaticity
x M19 resolved radiometric scene intensity
x limb-darkening profile
x granulation/activity modulation

The physical photospheric core remains the surface/GI emitter.

## Point-proxy representation

The old StellarPointProxy used a hard-coded warm tint and also multiplied already inverse-square point irradiance by projected photosphere area, effectively over-attenuating distant stars.

M26 replaces it with a fixed-pixel core + halo representation.

Point-proxy brightness uses:

- M19 inverse-square integrated irradiance;
- reciprocal raster-area normalization;
- temperature-derived color;
- compact core profile;
- authorable glare halo;
- low-weight orthogonal diffraction-spike profile.

Because the glare raster radius is screen-space stable and brightness is normalized by its area, integrated proxy energy follows the M19 inverse-square irradiance rather than acquiring another distance-squared factor.

## Glare and diffraction

Glare/diffraction are presentation effects in the HDR scene buffer.

They are not:

- RadiativeEmitter luminosity;
- CelestialLightingService source power;
- GI source area;
- Photosphere radius.

This prevents a larger glare radius from lighting the world more strongly.

## Shared HDR path

All M26 visual output is written before the existing DisplayResolve stage.

The existing sequence remains:

HDR scene-linear celestial output
-> view exposure scale
-> tone map
-> LUT correction

There is currently no separate generic bloom module in the V0.0.6 branch.

M26 therefore does not invent one inside stellar authority.

Its HDR corona/glare is intentionally compatible with a later global bloom/flare stage, which must consume scene luminance rather than become a light source.

## Surface / GI emission

DrawSurfaceData only writes the physical photospheric core as stellar emission.

Its RGB comes from the same temperature-derived color and its scale comes from M19 resolved radiometry.

Chromosphere, corona and glare are deliberately excluded from the surface-emission buffer.

Thus downstream shared lighting/GI sees the radiative star rather than a screen-space flare proxy.

## Representation transitions

M14/M18 still own representation selection and overlap.

M26 only specializes stellar appearance within those representations.

The ladder remains:

resolved stellar disc
-> analytic stellar disc
-> StellarPointProxy

No stellar representation identity is persisted.

## Recipes

Star recipes now copy their deterministic recipe seed into Photosphere Activity Seed.

Generated stars therefore have stable but distinct activity/granulation patterns while all M26 fields remain ordinary editable Inspector properties.

## Studio diagnostics

StudioStellarDiagnostics exposes:

- body;
- stellar appearance fingerprint;
- effective temperature;
- derived linear RGB;
- projected photospheric radius;
- active M14 representation;
- resolved-disc scene intensity;
- point-proxy scene intensity.

Diagnostics are regenerated from the current viewport target and cannot become stale after switching away from a star.

## Validation

M26 regression coverage includes:

- cool blackbody appearance is red-dominant;
- hot blackbody appearance is blue-dominant;
- solar-temperature chromaticity remains bright in red/green;
- center-to-limb intensity decreases with limb darkening;
- deterministic granulation varies over surface direction;
- chromosphere and corona finite radial support;
- stellar appearance fingerprints;
- semantic appearance edits changing only M26 fingerprint;
- visual-only edits preserving M19 luminosity, temperature and radius;
- physical temperature edits changing both M19 radiometry and M26 color.

## Intentional limits

M26 does not yet implement:

- spectral line synthesis;
- resolved convection simulation;
- magnetic-loop geometry;
- CME simulation;
- prominences as explicit 3D structures;
- relativistic stellar rotation;
- gravitational lensing;
- a global scene bloom pipeline.

Those can be added later without changing the M19/M26 authority split.
