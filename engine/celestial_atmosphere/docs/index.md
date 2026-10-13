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
  "engine/celestial_atmosphere/include/orbit/celestial_atmosphere/AtmosphereRefraction.hpp",
  "engine/celestial_atmosphere/include/orbit/celestial_atmosphere/Atmosphere.hpp",
  "engine/celestial_atmosphere/include/orbit/celestial_atmosphere/AtmosphereRenderer.hpp",
  "engine/celestial_atmosphere/include/orbit/celestial_atmosphere/SkyIrradiance.hpp",
  "engine/world_model/include/orbit/world_model/CelestialAtmosphereBinding.hpp",
  "engine/studio_ui/src/StudioViewportRenderer.cpp",
]
symbols = ["RefractDirection", "AtmosphereParameters", "AtmosphereRenderer", "GpuAtmosphereLuts", "ResolveAtmosphereBody", "SunTransmittanceAt"]
invariants = [
  "AtmosphereRefraction.hpp derives refraction from AtmosphereParameters alone: radius = bottomRadiusMeters, scale height = rayleighScaleHeightMeters, surface refractivity = 2.78e-4 x (green Rayleigh scattering / 13.558e-6), which assumes an Earth-like gas (Rayleigh ~ N a^2, refractivity ~ N a). RefractDirection rotates a true direction toward the local vertical by the table's bending in the vertical plane; an object hidden even after refraction, an observer in space or a ducting atmosphere get the true direction back. It is applied in StudioViewportRenderer when ORBIT_REFRACT_SUN=1 (read once, default off): right after ResolveStudioDirectLight, from the camera's body-centric position and the previous frame's atmosphere presentation, it replaces studioDirectLight.directionBody with the apparent direction (unless the cloud-lab sun override is active, the observer is outside the atmosphere, or the sun stays hidden). directionBody is the one sun direction the atmosphere pass and sky, direct light, proxy shadow, glass, SDF light and clouds all read, so a single refraction keeps them consistent; refracting at any other single consumer would leave shadows and the disc up to 0.5 degrees off the light. Unverified: compiled with -fsyntax-only, never run or seen on screen; the first frame after a body change is unrefracted.",
  "The Atmosphere capability is the only persistent atmosphere authority: there is exactly one enabled Physical Scattering capability per body (duplicates are rejected) and ResolveAtmosphereBody maps it to AtmosphereParameters. Do not add a second atmosphere representation or cache.",
  "Nothing Earth-specific is baked into the runtime: body radius, atmosphere radius, density profiles, scattering coefficients and LUT resolutions are explicit inputs (the struct defaults are only an Earth-like starting point).",
  "The body reference radius is the lower atmosphere boundary; altitude is measured from bottomRadiusMeters.",
  "LUTs are derived products: they are rebuilt from the fingerprint, uploaded through an explicit render-graph pass and never written to the project database.",
  "The same static LUT authority serves ground sky and orbital limb: observers outside the atmosphere are not clamped to the top radius.",
  "Sunlight on surfaces and clouds comes from the same transmittance LUT the sky is made of (SunTransmittanceAt), so lit surfaces receive the same reddened sunlight as the sky.",
  "bypass_atmosphere skips the atmosphere pass for a view (and, as documented, the clouds drawn after it); it is a diagnostic flag and must be restored.",
  "The aerial-perspective raymarch uses ceil(pathLength / 300 m) + 3 steps clamped to 4..40. Long space-to-ground paths distribute samples continuously around nearest approach with a two-sided cubic map; there must be no threshold where sample positions jump with camera altitude or ray direction.",
]
related = ["/rendering/clouds", "/rendering/lighting"]
depends_on = ["/rendering"]
used_by = ["/rendering/clouds", "/rendering/lighting"]
verify = [
  "ctest -R Orbit.CelestialAtmosphere (Atmosphere and SkyIrradiance tests), Orbit.CelestialAtmosphereBinding and Orbit.AtmospherePropertySolver.",
  "orbit_view_terrain_layers_set(bypass_atmosphere=true): the sky, haze and limb disappear; restoring false brings them back without a restart.",
]
verified = "74906ec"

[routes]
"how the LUTs are built, fingerprints, what invalidates what, sky view, limb" = "lut-pipeline"
"how sky light reaches surfaces (spherical harmonics, sky frame, enclosed spaces)" = "sky-irradiance"
"author an atmosphere from pressure/composition, presets, provenance, explicit vs derived values" = "authoring-solver"
"research baseline and intentional limits of the physical model" = "/legacy/tree-history-research-v006-physical-atmosphere"
"rocky-planet recipes with atmospheres" = "/legacy/tree-history-research-v006-atmosphere-authoring-solver/recipes"
+++

## Where it lives

| Concern | Location |
| --- | --- |
| physical model, LUT builders, fingerprints, GPU LUT textures | `engine/celestial_atmosphere/Atmosphere.hpp` |
| per-pixel composite | `AtmosphereRenderer.hpp/.cpp` |
| sky irradiance for lighting | `SkyIrradiance.hpp/.cpp` |
| capability -> parameters | `engine/world_model/CelestialAtmosphereBinding.hpp` |
| authoring solver and provenance | `engine/world_model/AtmospherePropertySolver.hpp`, `PropertyProvenance*.hpp` |
| per-view cache, diagnostics, pass wiring | `engine/studio_ui/src/StudioViewportRenderer.cpp` (`AtmospherePresentation`, `StudioAtmosphereDiagnostics`) |

## Terminator / orbital sampling continuity

The atmosphere shader uses bounded closest-approach quadrature for orbital and tangent rays. A previous implementation switched abruptly between one-sided and two-sided cubic distributions when closest approach moved within 2.5% of the ray interval's endpoints. Those formulas did not connect: changing a pixel angle infinitesimally could move a sample boundary by about 10% of the view-ray path, which produced nested dark rings when looking towards dawn. The corrected distribution uses a continuous closest-approach split, with the two square-root weights calculated once per ray, and two cubic branches that agree at the split. GPU work remains 4..40 samples. `AtmosphereTests.cpp` checks ordered boundaries, the two endpoints, and continuity across the former thresholds. This removes **one deterministic source** of bands, not all possible aliasing: an opaque-planet shadow front crossing a wide integration segment still needs segment-integrated visibility for accurate wide-angle twilight, and the spherical multiple-scattering LUT still approximates the ellipsoid.

## Experimental terrain-aware sunlight

The per-view `terrain_occluded_atmosphere` option evaluates direct single-scattering visibility from an asynchronous disposable, body-fixed `TerrainSource` field, leaving multiple scattering unchanged. The field is keyed by terrain identity/revision, body and radius: neither camera movement nor timewarp nor changing stellar direction rebuilds geometry. Until the field completes, finite-disc analytic planet occultation is used. Sparse view-ray knots bound the GPU cost. The switch is off by default in Lighting / Renderer > Atmosphere and is also accessible via `view.terrain_layers_set` and MCP. This first implementation intentionally leaves dynamic cloud optical-depth sharing and terrain direct-surface shading to later work; the existing cloud god-ray correction remains active.

## Frame order (per view)

The render graph adds the atmosphere pass, then the cloud passes; clouds are composited after the atmosphere and
apply their own aerial perspective over the camera-to-cloud distance (`/rendering/clouds/raymarch`). Direct and
indirect lighting use the atmosphere's sun transmittance and sky irradiance (`/rendering/lighting/sky-cache-fill`).
