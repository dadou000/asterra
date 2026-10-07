# Orbit V0.0.6 — Physical Atmosphere Model and LUT Pipeline

Status: M21 implementation baseline.

## Research basis

The M21 architecture follows two established atmosphere-rendering lines:

- Bruneton/Neyret precomputed atmospheric scattering for physically dimensioned density, extinction, transmittance and higher-order scattering;
- Hillaire's production-ready low-dimensional LUT approach for scalable ground-to-space rendering and compact multiple-scattering approximation.

Orbit does not copy Earth-only texture-coordinate constants. Body radius, atmosphere radius, density profiles, scattering coefficients and LUT resolutions are explicit inputs.

## Semantic authority

The Atmosphere capability is the only persistent atmosphere authority.

It now exposes:

- top radius [m];
- Rayleigh scattering RGB [1/m];
- Rayleigh scale height [m];
- Mie scattering RGB [1/m];
- Mie extinction RGB [1/m];
- Mie scale height [m];
- Mie anisotropy g;
- absorption extinction RGB [1/m];
- absorption layer center height [m];
- absorption layer half width [m];
- ground albedo RGB.

The body reference radius is the lower atmosphere boundary.

WorldModel::ResolveAtmosphereBody maps one enabled Physical Scattering capability into AtmosphereParameters.

Duplicate enabled atmosphere capabilities are rejected.

## Density model

Altitude is measured from bottomRadiusMeters.

Rayleigh density:

```
rho_R(h) = exp(-h / H_R)
```

Mie density:

```
rho_M(h) = exp(-h / H_M)
```

The baseline absorber uses a triangular vertical layer centered at an explicit altitude with explicit half width.

The model remains parameterized; M22 may derive these parameters from pressure/composition authority without changing the runtime atmosphere contract.

## Extinction and scattering

Local scattering is:

```
sigma_s =
    beta_R * rho_R +
    beta_M_s * rho_M
```

Local extinction is:

```
sigma_t =
    beta_R * rho_R +
    beta_M_e * rho_M +
    beta_abs * rho_abs
```

Mie extinction is validated to be greater than or equal to Mie scattering per channel.

Ground albedo is constrained to [0,1].

## Transmittance LUT

The transmittance LUT is a 2D function of:

- radius within the atmosphere;
- direction cosine relative to the local radial direction.

Optical depth is evaluated with deterministic trapezoidal integration through the physical atmosphere shell.

```
T = exp(- integral sigma_t ds)
```

Rays blocked by the ground return zero transmittance to the top boundary.

The radius mapping is derived from bottom/top radius rather than Earth-specific constants.

## Multiple-scattering LUT

M21 uses a low-dimensional isotropic multiple-scattering response inspired by Hillaire's production model.

At each radius:

1. deterministic spherical directions estimate mean escape/transmission;
2. local scattering albedo determines how much energy remains available for additional scattering;
3. the recirculating component is accumulated as a bounded geometric series;
4. source-direction transmittance modulates the resulting response.

Ground-intersecting directions include the authored ground albedo.

The result is stored as a compact 2D RGB response over:

- radius;
- source zenith cosine.

This is deliberately a reusable approximation seam rather than a hidden Earth preset.

## Sky-view LUT

Sky view depends on:

- the static atmosphere products;
- observer radius;
- body-fixed source direction;
- incident irradiance from M20.

Each sky ray integrates:

- view-path transmittance;
- source transmittance;
- Rayleigh phase;
- Henyey-Greenstein Mie phase;
- local single scattering;
- isotropic multi-scattering response.

The LUT is parameterized by:

- view zenith;
- relative azimuth to the source.

Its values are scene-linear radiance-like RGB quantities prior to the M19 display transform.

## Ground-to-space geometry

Observers inside the atmosphere integrate from the observer to the nearest physical boundary.

Observers outside the atmosphere are not clamped to the top radius.

The view ray is intersected with the top atmosphere sphere and only the actual segment through the atmospheric shell is integrated.

If the ray reaches the ground before leaving the shell, the integration terminates at the ground.

This allows the same AtmosphereParameters and static LUT authority to generate both:

- ground sky;
- orbital limb.

## Revision fingerprints

AtmosphereFingerprint includes all physical parameters and LUT-quality settings.

Static products:

- transmittance;
- multi-scattering;

share this fingerprint.

AtmosphereSkyFingerprint additionally includes:

- observer radius;
- normalized source direction;
- incident irradiance.

Therefore:

- composition/profile edits invalidate static + sky products;
- camera altitude/source motion/eclipse changes invalidate sky view only.

## GPU products

All three LUTs upload as RGBA16F RHI textures.

GpuAtmosphereLuts owns:

- transmittance texture;
- multi-scattering texture;
- sky-view texture;
- host-visible staging resources;
- upload state.

ReplaceSkyView replaces only the view-dependent GPU texture.

Static textures remain resident until the static atmosphere fingerprint changes.

## Studio integration

Studio maintains a per-view AtmospherePresentation cache.

For targeted bodies with an enabled atmosphere:

- semantic authority resolves through WorldModel;
- static LUTs build only when AtmosphereFingerprint changes;
- sky view builds when AtmosphereSkyFingerprint changes;
- M20 eclipse-attenuated irradiance and body-fixed source direction feed SkyViewInput;
- an explicit render-graph pass uploads the derived textures;
- no LUT is persisted to the project database.

StudioAtmosphereDiagnostics exposes:

- body;
- static fingerprint;
- sky fingerprint;
- observer radius/altitude;
- direct irradiance;
- all LUT dimensions.

M32 can expose this without changing atmosphere authority.

## Generated systems

Rocky-planet recipes now author atmosphere top radius relative to the generated planet radius instead of relying on an Earth-sized absolute default.

## Validation

Deterministic coverage includes:

- density falloff with altitude;
- transmittance/extinction range coverage;
- static fingerprint reproducibility;
- ground sky with non-zero radiance;
- orbital-limb sky with non-zero radiance from the same static authority;
- observer/light-state sky fingerprint invalidation;
- physical-profile static fingerprint invalidation;
- semantic body/capability binding and revision propagation.

## Intentional limits

M21 does not yet provide:

- pressure/composition-to-scattering derivation;
- expert/profile provenance solving;
- wavelength-resolved spectral transport;
- weather-dependent aerosol fields;
- cloud scattering;
- aerial-perspective volume texture;
- atmospheric refraction;
- final sky/limb composition shader in every viewport path.

M22 owns composition/profile/pressure/aerosol/absorber authoring and solver integration. Subsequent atmosphere/cloud milestones can consume the same LUT products without creating another atmosphere authority.
