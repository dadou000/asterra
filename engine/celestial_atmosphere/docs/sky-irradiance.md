+++
path = "/rendering/atmosphere/sky-irradiance"
title = "Sky irradiance (sky frame and spherical harmonics)"
kind = "concept"
status = "stable"
owner_module = "OrbitCelestialAtmosphere"
summary = """
The sky-view table is projected to second-order real spherical harmonics in a sky frame (+Z observer zenith, sun at \
azimuth 0); EvaluateSkyIrradiance returns cosine-convolved irradiance for any surface normal, clamping negative lobes \
from the order-2 truncation. Lighting consumes it as the sky summary for sky fill and radiance-cache cells."""
keywords = ["sky irradiance", "spherical harmonics", "sh", "sky frame", "ambient", "sky light", "cosine convolved", "zenith", "sky summary"]
sources = [
  "engine/celestial_atmosphere/include/orbit/celestial_atmosphere/SkyIrradiance.hpp",
  "engine/celestial_atmosphere/src/SkyIrradiance.cpp",
  "engine/celestial_atmosphere/tests/SkyIrradianceTests.cpp",
  "engine/lighting/include/orbit/lighting/RadianceEstimator.hpp",
]
symbols = ["SkySphericalHarmonics", "ProjectSkyViewToSphericalHarmonics", "EvaluateSkyIrradiance", "MakeSkyFrameBasis", "SkyFrameBasis", "SkyFrameSunDirection"]
invariants = [
  "The sky frame is +Z = observer zenith, +X = horizontal direction toward the sun (any horizontal direction when the sun is at zenith or nadir), +Y completes a right-handed set; SkyFrameBasis::ToBody/FromBody convert between it and the body frame.",
  "Coefficients (9 RGB, order 2) are in the sky-view table's own radiance units; EvaluateSkyIrradiance returns radiance units x steradian for a normal given in the SKY frame, so callers must rotate world normals with the basis first.",
  "Negative lobes from the order-2 truncation are clamped to zero.",
  "Without sunlight the sky summary is zero, so every fill derived from it (radiance-cache sky channel, proxy sky fill) vanishes on the night side and in space.",
]
related = ["/rendering/lighting/sky-cache-fill", "/rendering/atmosphere/lut-pipeline"]
depends_on = ["/rendering/atmosphere/lut-pipeline"]
used_by = ["/rendering/lighting/sky-cache-fill", "/rendering/lighting/proxy-sun-shadow"]
verify = ["ctest -R Orbit.CelestialAtmosphereSkyIrradiance."]
verified = "b0a0de7f"

[[diagnose]]
symptom = "sky-lit surfaces are lit from the wrong side, or lighting rotates oddly while walking"
steps = [
  "Check the normal is expressed in the sky frame before EvaluateSkyIrradiance: use MakeSkyFrameBasis(observerUpBody, sunDirectionBody).FromBody(normalBody).",
  "Check the sun direction fed to the sky view is the sky-frame direction from SkyFrameSunDirection, not a body-fixed vector.",
  "If lighting jumps while walking, the zenith cosine snap (1/512) or the observer-radius quantisation was bypassed.",
]
docs = ["/rendering/atmosphere/lut-pipeline"]
+++

Consumers: `engine/lighting` (`RadianceEstimator`, `RadianceClipmap`, `DirectLighting`) and
`StudioViewportRenderer.cpp` use the projected coefficients; the cell-level sky fill is described in
`/rendering/lighting/sky-cache-fill`.
