# V0.0.6 M29 — Magnetosphere / Aurora Baseline

## Purpose

M29 establishes a replaceable semantic/runtime seam for magnetic-field and auroral
presentation. It deliberately does not make Atmosphere the owner of magnetospheric
state and does not attempt full magnetohydrodynamics.

## Semantic authority

The Magnetosphere / Aurora capability owns explicit body-fixed parameters:

- magnetic dipole axis;
- equatorial surface field strength;
- incident solar-wind direction;
- subsolar magnetopause standoff in body radii;
- magnetopause flaring exponent and maximum tail extent;
- solar-wind dynamic pressure and interplanetary Bz driver values;
- normalized activity;
- auroral oval latitude/width;
- auroral altitude interval;
- scene-linear HDR emission color, intensity, structure and deterministic seed.

The atmosphere capability is optional. This keeps particle/magnetic authority
separate from atmospheric composition and allows airless-body auroral/plasma
presentation experiments without schema changes.

## Baseline field model

The runtime baseline evaluates an ideal dipole field:

B(r) = B_eq (R/r)^3 [3 m (m dot r_hat) - m_hat]

where the authored equatorial field at one reference radius defines scale. This is
a presentation/sampling baseline, not an internal dynamo solver.

## Magnetopause approximation

The magnetopause envelope follows a Shue-style axisymmetric form:

r(theta) = r0 [2 / (1 + cos(theta))]^alpha

and is clamped by an authored maximum tail extent. The incident wind direction is
explicit, so no world-axis or Earth-specific sun direction is hidden in the model.

The future field/MHD service can replace this approximation while preserving the
same semantic capability and runtime resolver.

## Aurora baseline

Activity moves the nominal auroral oval equatorward and modulates oval width.
The renderer builds two body-fixed altitude-spanning curtain ribbons around the
magnetic axis.

Presentation is:

- deterministic from semantic parameters and seed;
- HDR scene-linear emission;
- independent of diffuse/direct illumination;
- near/far LOD aware;
- body-rotation compatible because geometry is authored in body-fixed space;
- not encoded as a solid surface.

The baseline color is an authorable presentation value rather than an atmosphere
chemistry result. A later precipitation + atmospheric-species emission solver can
derive spectral/color output without changing the magnetosphere schema.

## Module split

- Orbit::CelestialMagnetosphere — semantic-independent field, envelope, oval and
  CPU curtain derivation.
- Orbit::CelestialMagnetosphereRender — disposable RHI auroral curtain GPU
  presentation.
- WorldModel::ResolveMagnetosphere — semantic-to-runtime binding.
- StudioViewportRenderer — per-viewport near/far cache and diagnostics.

This split prevents the core magnetic model from depending on RHI/Studio.

## Validation

Current deterministic coverage verifies:

- north/south oval generation;
- dipole equatorial field normalization;
- magnetotail extent greater than subsolar nose;
- auroral oval localization;
- activity-driven equatorward motion;
- HDR curtain emission;
- deterministic mesh topology/fingerprints;
- semantic binding without an Atmosphere capability;
- fingerprint invalidation after semantic edits.

## Remaining M29 acceptance work

- pass the integrated Windows/Studio shader/build gate;
- add optional magnetopause/field-line diagnostic visualization;
- expose auroral emissive volume to the shared indirect-light/radiance-source seam
  without pretending the curtain is an opaque surface;
- validate ground/orbit/far transition behavior on a magnetic planet and moon.
