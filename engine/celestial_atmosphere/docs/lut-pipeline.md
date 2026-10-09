+++
path = "/rendering/atmosphere/lut-pipeline"
title = "LUT pipeline, fingerprints and invalidation"
kind = "concept"
status = "stable"
owner_module = "OrbitCelestialAtmosphere"
summary = """
Static LUTs (transmittance 128x32, multiple scattering 32x16) share AtmosphereFingerprint; the sky-view LUT (128x64) \
adds observer radius, source direction and incident irradiance (AtmosphereSkyFingerprint). Composition/profile edits \
invalidate everything; camera altitude, source motion and eclipse changes invalidate the sky view only. GPU LUTs are \
RGBA16F and ReplaceSkyView swaps just the view-dependent texture."""
keywords = ["lut", "transmittance", "multiple scattering", "sky view", "fingerprint", "invalidation", "limb", "ground to space", "rhi texture", "sky observer radius", "quantized", "rebuild"]
sources = [
  "engine/celestial_atmosphere/include/orbit/celestial_atmosphere/Atmosphere.hpp",
  "engine/celestial_atmosphere/src/Atmosphere.cpp",
  "engine/celestial_atmosphere/include/orbit/celestial_atmosphere/SkyIrradiance.hpp",
  "engine/studio_ui/src/StudioViewportRenderer.cpp",
  "engine/celestial_atmosphere/tests/AtmosphereTests.cpp",
]
symbols = ["AtmosphereLutConfig", "BuildStaticLuts", "BuildSkyView", "AtmosphereFingerprint", "AtmosphereSkyFingerprint", "SkyViewInput", "ReplaceSkyView", "SkyFrameSunDirection", "QuantizedSkyObserverRadius"]
invariants = [
  "Density is exponential with altitude above bottomRadiusMeters: rho_R = exp(-h/H_R), rho_M = exp(-h/H_M); the absorber is a triangular vertical layer with an explicit centre height and half width.",
  "sigma_s = beta_R rho_R + beta_M_s rho_M; sigma_t = beta_R rho_R + beta_M_e rho_M + beta_abs rho_abs. Mie extinction must be >= Mie scattering per channel and ground albedo must be in [0, 1] (both validated).",
  "Transmittance is a 2D LUT over (radius within the shell, direction cosine to the local radial), integrated deterministically with trapezoids; rays blocked by the ground return zero transmittance to the top boundary; the radius mapping derives from bottom/top radius, not Earth constants.",
  "Multiple scattering is a low-dimensional isotropic response over (radius, source zenith cosine) inspired by Hillaire: a bounded geometric series of the recirculating component, with the authored ground albedo for ground-intersecting directions. It is an approximation seam, not an Earth preset.",
  "The sky view is parameterised by view zenith and relative azimuth to the source and holds scene-linear radiance-like RGB before the display transform; each ray integrates view transmittance, source transmittance, Rayleigh and Henyey-Greenstein Mie phase, local single scattering and the multiple-scattering response.",
  "Observers inside the shell integrate to the nearest boundary; observers outside intersect the view ray with the top sphere and integrate only the actual segment, stopping at the ground if it is reached first.",
  "AtmosphereFingerprint covers every physical parameter and LUT-quality setting; AtmosphereSkyFingerprint adds observer radius, normalised source direction and incident irradiance. Composition/profile edits invalidate static + sky products; camera altitude, source motion and eclipse changes invalidate only the sky view.",
  "SkyViewInput::sunDirectionBody is the sun expressed in the SKY FRAME (+Z observer zenith, sun at azimuth 0), not a body-fixed direction; SkyFrameSunDirection builds it and snaps the zenith cosine to 1/512 so walking over the surface does not change the sky fingerprint every frame.",
  "The observer altitude used for the sky-view table is snapped to about 2 % logarithmic steps (QuantizedSkyObserverRadius, kLogStep 0.02), so climbing or descending rebuilds the table a few times per altitude decade instead of every frame; the table varies smoothly so the error is a fraction of a pixel.",
  "All three LUTs upload as RGBA16F textures; GpuAtmosphereLuts::ReplaceSkyView replaces only the view-dependent texture and the static textures stay resident until the static fingerprint changes.",
  "Studio keeps a per-view AtmospherePresentation cache: static LUTs rebuild only when AtmosphereFingerprint changes, the sky view only when AtmosphereSkyFingerprint changes, using M20 eclipse-attenuated irradiance; nothing is persisted.",
]
related = ["/rendering/atmosphere/sky-irradiance", "/rendering/atmosphere/authoring-solver"]
depends_on = ["/rendering/atmosphere"]
verify = [
  "ctest -R Orbit.CelestialAtmosphere: density falloff, transmittance/extinction ranges, fingerprint reproducibility, non-zero ground sky and orbital limb from the same static authority, observer/light-state invalidation of the sky fingerprint only, physical-profile invalidation of the static fingerprint.",
  "StudioAtmosphereDiagnostics: body, static fingerprint, sky fingerprint, observer radius/altitude, direct irradiance and all LUT dimensions.",
  "Climbing from the ground to orbit rebuilds the sky view a few times per altitude decade, not every frame (profile with orbit_profiler_capture: atmosphere.build_sky_view scope).",
]
verified = "db348ce94035630577b705cffe0c69c6f8a6061f"

[[diagnose]]
symptom = "sky or limb looks wrong after editing atmosphere properties (stale or unchanged)"
steps = [
  "Any successful edit of a physical coefficient must change AtmosphereFingerprint (the solver writes the same properties ResolveAtmosphereBody reads); if the sky did not update, check that the edit reached the capability properties and that the view's AtmospherePresentation observed the new static fingerprint.",
  "Read StudioAtmosphereDiagnostics for the view: compare the static and sky fingerprints before and after the edit.",
  "If only the sky fingerprint changed, the edit was an observer/light change, which correctly leaves the static LUTs alone.",
]
docs = ["/rendering/atmosphere/authoring-solver"]

[[diagnose]]
symptom = "frame hitch or repeated CPU work while changing altitude"
steps = [
  "Capture a trace (orbit_profiler_capture) and look for atmosphere.build_sky_view slices every frame.",
  "The observer radius must be quantised (QuantizedSkyObserverRadius, about 2 % steps) and the sun direction zenith-snapped (1/512); a missing snap makes the sky fingerprint change every frame.",
  "BuildSkyView builds its rows in parallel; if it is serial, that regressed.",
]
docs = ["/rendering/terrain/clipmaps/rebuild-hitches"]
+++

## Defaults (a starting point, not a contract)

`AtmosphereParameters{}` is Earth-like: bottom 6,371 km, top 6,471 km, Rayleigh scale height 8 km, Mie scale height 1.2 km
and g = 0.8, absorber centred at 25 km with a 15 km half width, ground albedo 0.1. `AtmosphereLutConfig{}`:
transmittance 128x32, multiple scattering 32x16, sky view 128x64, with 64 optical-depth steps, 32 multi-scattering
directions and 48 sky-view steps.

## Intentional limits (stated at the M21 baseline; verify before relying)

No pressure/composition derivation at runtime (that is the authoring solver), no wavelength-resolved spectral transport,
no weather-dependent aerosol field, no atmospheric refraction, no aerial-perspective volume texture. Aerial perspective
for clouds is integrated inside the cloud shader over the camera-to-cloud distance with the same LUTs
(`/rendering/clouds/raymarch`). Source: `/legacy/tree-history-research-v006-physical-atmosphere`.
