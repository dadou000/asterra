# V0.0.6 M28 — Airless / Irregular / Small-Body Rendering Baseline

## Scope

M28 treats asteroids, comet nuclei and moonlets as ordinary celestial bodies with a
specialized derived appearance/shape capability. It does not introduce a new body
class and does not require the planetary toroidal terrain stack.

## Shape model

The semantic Reference Shape remains the physical scale authority. The Small Body
Appearance capability contributes a disposable directional radius multiplier:

- authored axis scale provides elongated/triaxial forms;
- low-frequency deterministic structure produces large lobes and non-hydrostatic
  silhouettes;
- crater bowls and rims perturb the radial field;
- the field is cube-sphere sampled so later mesh/ray representations can share one
  deterministic source.

The radial product is derived, fingerprinted and disposable.

## Regolith appearance

A separate cube-sphere appearance product provides:

- regolith and fresh-material linear albedo;
- deterministic fine color variation;
- crater-correlated fresh exposure;
- high diffuse roughness;
- no ocean, atmosphere or emission assumptions.

This allows the same appearance authority to feed smooth, disc and future
irregular-mesh representations.

## Rough-surface photometry

The baseline uses a compact Hapke-inspired particulate response:

- Lommel-Seeliger single-scattering term for dark airless surfaces;
- configurable opposition surge strength and angular width;
- configurable single-scattering albedo;
- macroscopic-roughness shadowing.

The implementation deliberately exposes these controls instead of hiding
Earth/Moon-specific constants. It is a rendering baseline rather than a claim of
full laboratory Hapke inversion.

## LOD contract

Small bodies continue to use the common M14 representation resolver. They do not
gain a special distance ladder. Irregular geometry/photometry must fade into the
ordinary smooth/disc/point representations with the same overlap and hysteresis
rules, preserving apparent area and phase response.

## Comet baseline

"Comet Nucleus" is a Small Body class. The existing Comet Tail capability remains
separate so nucleus surface/shape authority is not coupled to future volatile,
dust-tail or plasma-tail simulation.

## Validation

Deterministic tests cover:

- stable shape/appearance fingerprints;
- non-spherical silhouette range;
- elongated-axis response;
- opposition response stronger at near-zero phase;
- fingerprint invalidation when crater authority changes.

The remaining M28 integration work is to make the live far renderer consume the
directional shape and particulate photometry parameters directly and verify the
smooth/disc transition visually.
