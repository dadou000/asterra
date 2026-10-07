# Orbit V0.0.6 — Atmosphere Authoring and Solver Integration

Status: M22 implementation baseline.

## Purpose

M22 adds a provenance-aware authoring layer above the M21 physical atmosphere runtime.

There is still only one persistent Atmosphere capability and one set of M21 runtime coefficients.

The solver derives those coefficients from simpler pressure/composition/aerosol controls when requested.

Expert users can retain direct authority over the exact M21 coefficients.

## Authoring modes

Atmosphere exposes two authoring modes:

- Derived Composition
- Expert Coefficients

Derived Composition runs the M22 solver.

Expert Coefficients leaves the M21 runtime coefficients under direct author authority and performs no derivation.

The shared Inspector remains the exact numeric editing surface.

## Simple authoring inputs

M22 adds:

- surface pressure [Pa];
- surface temperature [K];
- surface gravity [m/s2];
- N2 fraction;
- O2 fraction;
- Ar fraction;
- CO2 fraction;
- aerosol optical depth at 550 nm;
- aerosol single-scattering albedo;
- aerosol Angstrom exponent;
- aerosol scale height [m];
- absorber column scale.

Raw M21 coefficients remain available under Advanced Properties.

## Presets

The unified Celestial panel provides ordinary editable presets:

- Earth-like;
- Thin CO2;
- Dense CO2;
- Dry Nitrogen.

A preset writes the simple authoring fields as ordinary semantic properties and records Procedural/Solved provenance.

It immediately runs the same solver used by manual Derived Composition authoring.

No preset creates a hidden runtime type.

## Surface gravity

When body mass and radius are available and surface-gravity authority is writable:

```
g = G M / R^2
```

The result is stored with Derived/Solved provenance.

An explicit/imported/locked gravity value is preserved.

## Mean molar mass

The baseline bulk composition supports N2/O2/Ar/CO2.

Fractions are normalized before solving.

Mean molar mass is:

```
Mbar =
    x_N2  * 0.0280134 +
    x_O2  * 0.0319988 +
    x_Ar  * 0.039948 +
    x_CO2 * 0.0440095
```

in kg/mol.

M22 intentionally does not claim to be a full equilibrium chemistry solver.

Additional species can extend this authoring layer later without changing the M21 runtime contract.

## Rayleigh scale height

The gas scale height baseline is:

```
H_R = R_gas T / (Mbar g)
```

where:

- T is surface temperature;
- Mbar is mean molar mass;
- g is surface gravity.

## Rayleigh scattering

The M21 RGB Rayleigh coefficients are derived from the accepted Earth-like spectral baseline, scaled by:

- pressure/temperature number density;
- a bulk-composition refractivity weighting.

The baseline composition weighting is an engineering approximation intended for robust arbitrary fictional atmospheres.

Exact spectral or laboratory-derived coefficients remain first-class expert inputs and may be locked/imported.

The solver never overwrites those authorities.

## Aerosol/Mie derivation

For RGB representative wavelengths 680/550/440 nm:

```
AOD(lambda) =
    AOD_550 * (lambda / 550 nm)^(-Angstrom)
```

With exponential aerosol scale height H_M:

```
beta_M_ext(lambda) =
    AOD(lambda) / H_M
```

Mie scattering is:

```
beta_M_scat =
    beta_M_ext * singleScatteringAlbedo
```

Mie scale height follows the authored aerosol scale height.

The derived baseline anisotropy is g=0.8.

Exact phase asymmetry remains an Advanced expert coefficient.

## Absorber derivation

The baseline M21 absorber spectral coefficients are multiplied by the authored absorber column scale.

The vertical triangular absorber layer is scaled from the solved Rayleigh scale height:

```
center = 3.125 H_R
halfWidth = 1.875 H_R
```

This reproduces the M21 Earth-like 25 km / 15 km baseline at H_R ~= 8 km while remaining radius/composition independent.

Exact absorber coefficients/profile remain expert controls.

## Atmosphere top radius

When writable:

```
topRadius =
    bodyRadius +
    max(
        10 H_R,
        12 H_M,
        absorberCenter + 2 absorberHalfWidth,
        1 m)
```

Recipe-authored explicit atmosphere extents remain authoritative and are not overwritten.

## Provenance storage

M22 adds a reusable PropertyProvenanceStore adapter.

One ordinary Property Provenance child record is keyed by the stable target PropertyId string.

The adapter reads/writes:

- source mode;
- solve state;
- source object/asset/property;
- uncertainty.

A stored property that has no provenance metadata is conservatively treated as:

- Explicit;
- Locked.

This prevents legacy/manual expert values from being silently overwritten.

## Solver write policy

For each predicted M21 coefficient:

- Default/Derived/Procedural writable authority may be solved;
- Explicit authority is preserved;
- Imported authority is preserved;
- Locked authority is preserved;
- disagreement is returned as a solver conflict rather than silently overwritten.

Derived writes receive:

- SourceMode = Derived;
- SolveState = Solved;
- an explanation string describing the derivation.

## Shared Inspector behavior

Manual edits through the normal Properties Inspector now detect existing provenance records.

The same edit transaction:

1. writes the new value;
2. promotes its provenance to Explicit/Locked;
3. records "Manual Inspector edit" as the source description.

Undo restores both the value and its previous provenance atomically.

This is a generic M02 improvement and is not atmosphere-specific.

## Unified UI

No atmosphere-specific editor or launcher was added.

The existing Celestial panel now contains an Atmosphere Solver section when the selected body owns an Atmosphere capability.

It provides:

- preset buttons;
- Derived Composition mode;
- Expert Coefficients mode;
- Solve Derived Coefficients;
- derived/explicit/imported authority counts.

The shared Properties panel remains the exact numeric editor.

Advanced Properties reveals the raw M21 coefficients and Property Provenance child records remain inspectable through the ordinary hierarchy/Inspector path.

## Capability lifecycle

Provenance metadata is stored beneath its owning capability.

Removing a celestial capability now atomically removes Property Provenance metadata children before removing the capability itself.

Non-provenance child records still prevent accidental capability deletion.

## Recipes

Rocky-planet recipes with atmospheres now run through the same Earth-like preset + M22 solver path inside the existing recipe transaction.

The recipe-authored atmosphere extent remains explicit while pressure/composition-derived coefficients gain normal M02 provenance.

## M21 invalidation

The M22 solver writes directly to the same physical coefficient properties consumed by ResolveAtmosphereBody.

Therefore any successful solver edit changes the ordinary M21 AtmosphereFingerprint.

No parallel atmosphere representation or solver cache exists.

## Validation

Regression coverage includes:

- Earth-like derived scale height;
- Derived/Solved provenance persistence;
- raw expert property preservation;
- imported/locked property preservation;
- conflict reporting;
- one-transaction preset + solve undo/redo;
- pressure edit causing M21 fingerprint invalidation;
- Expert Coefficients mode performing no derivation;
- manual Inspector edit promotion to Explicit/Locked;
- atomic undo of both Inspector value and provenance.

## Intentional limits

M22 does not yet implement:

- full chemical equilibrium;
- condensation/cloud microphysics;
- line-by-line spectroscopy;
- arbitrary user-defined molecular species;
- pressure/temperature vertical profile tables;
- imported atmospheric profile file formats.

Those can extend the authoring/solver layer while continuing to resolve into the same M21 physical runtime parameters.
