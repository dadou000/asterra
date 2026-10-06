+++
path = "/rendering/atmosphere"
title = "Atmosphere"
kind = "subsystem"
status = "stable"
owner_module = "OrbitCelestialAtmosphere"
summary = """
One persistent authority (the Atmosphere capability on a body) resolves into AtmosphereParameters; static LUTs \
(transmittance, multiple scattering) and a view-dependent sky-view LUT are derived from it and never persisted. \
AtmosphereRenderer composites sceneColor * T + L per pixel from ground to orbit; the same transmittance LUT gives \
surfaces and clouds their reddened sunlight, and the sky view is projected to spherical harmonics for sky lighting. \
Authoring (pressure/composition -> coefficients) is a separate provenance-aware solver in world_model."""
keywords = ["atmosphere", "sky", "scattering", "rayleigh", "mie", "transmittance", "limb", "aerial perspective", "haze", "sunset", "atmosphere capability", "bypass_atmosphere"]
sources = [
  "engine/celestial_atmosphere/include/orbit/celestial_atmosphere/Atmosphere.hpp",
  "engine/celestial_atmosphere/include/orbit/celestial_atmosphere/AtmosphereRenderer.hpp",
  "engine/celestial_atmosphere/include/orbit/celestial_atmosphere/SkyIrradiance.hpp",
  "engine/world_model/include/orbit/world_model/CelestialAtmosphereBinding.hpp",
  "engine/studio_ui/src/StudioViewportRendererBase.cpp",
]
symbols = ["AtmosphereParameters", "AtmosphereRenderer", "GpuAtmosphereLuts", "ResolveAtmosphereBody", "SunTransmittanceAt"]
invariants = [
  "The Atmosphere capability is the only persistent atmosphere authority: there is exactly one enabled Physical Scattering capability per body (duplicates are rejected) and ResolveAtmosphereBody maps it to AtmosphereParameters. Do not add a second atmosphere representation or cache.",
  "Nothing Earth-specific is baked into the runtime: body radius, atmosphere radius, density profiles, scattering coefficients and LUT resolutions are explicit inputs (the struct defaults are only an Earth-like starting point).",
  "The body reference radius is the lower atmosphere boundary; altitude is measured from bottomRadiusMeters.",
  "LUTs are derived products: they are rebuilt from the fingerprint, uploaded through an explicit render-graph pass and never written to the project database.",
  "The same static LUT authority serves ground sky and orbital limb: observers outside the atmosphere are not clamped to the top radius.",
  "Sunlight on surfaces and clouds comes from the same transmittance LUT the sky is made of (SunTransmittanceAt), so lit surfaces receive the same reddened sunlight as the sky.",
  "bypass_atmosphere skips the atmosphere pass for a view (and, as documented, the clouds drawn after it); it is a diagnostic flag and must be restored.",
]
related = ["/rendering/clouds", "/rendering/lighting"]
depends_on = ["/rendering"]
used_by = ["/rendering/clouds", "/rendering/lighting"]
verify = [
  "ctest -R Orbit.CelestialAtmosphere (Atmosphere and SkyIrradiance tests), Orbit.CelestialAtmosphereBinding and Orbit.AtmospherePropertySolver.",
  "orbit_view_terrain_layers_set(bypass_atmosphere=true): the sky, haze and limb disappear; restoring false brings them back without a restart.",
]
verified = "b0a0de7f"

[routes]
"how the LUTs are built, fingerprints, what invalidates what, sky view, limb" = "lut-pipeline"
"how sky light reaches surfaces (spherical harmonics, sky frame, enclosed spaces)" = "sky-irradiance"
"author an atmosphere from pressure/composition, presets, provenance, explicit vs derived values" = "authoring-solver"
"research baseline and intentional limits of the physical model" = "/legacy/research-v006-physical-atmosphere"
"rocky-planet recipes with atmospheres" = "/legacy/research-v006-atmosphere-authoring-solver/recipes"
+++

## Where it lives

| Concern | Location |
| --- | --- |
| physical model, LUT builders, fingerprints, GPU LUT textures | `engine/celestial_atmosphere/Atmosphere.hpp` |
| per-pixel composite | `AtmosphereRenderer.hpp/.cpp` |
| sky irradiance for lighting | `SkyIrradiance.hpp/.cpp` |
| capability -> parameters | `engine/world_model/CelestialAtmosphereBinding.hpp` |
| authoring solver and provenance | `engine/world_model/AtmospherePropertySolver.hpp`, `PropertyProvenance*.hpp` |
| per-view cache, diagnostics, pass wiring | `engine/studio_ui/src/StudioViewportRendererBase.cpp` (`AtmospherePresentation`, `StudioAtmosphereDiagnostics`) |

## Frame order (per view)

The render graph adds the atmosphere pass, then the cloud passes; clouds are composited after the atmosphere and
apply their own aerial perspective over the camera-to-cloud distance (`/rendering/clouds/raymarch`). Direct and
indirect lighting use the atmosphere's sun transmittance and sky irradiance (`/rendering/lighting/sky-cache-fill`).
