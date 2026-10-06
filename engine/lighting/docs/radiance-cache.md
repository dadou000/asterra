+++
path = "/rendering/lighting/radiance-cache"
title = "Radiance clipmap cache (cells, residency, relighting)"
kind = "subsystem"
status = "stable"
summary = "A camera-centred clipmap of low-frequency diffuse irradiance cells (first-order spherical harmonics plus a sky-only channel) with budgeted relighting: 6 levels of 32^3 cells from 2 m cells scaling x4, each packed in a 96-byte GPU cell, with stale cells served until their refresh lands."
owner_module = "OrbitLighting"
keywords = ["radiance cache", "radiance clipmap", "irradiance", "spherical harmonics", "l1", "cell", "residency", "relight", "stale", "dark slabs", "96 bytes", "gpu cell", "sky visibility"]
sources = [
  "engine/lighting/include/orbit/lighting/RadianceClipmap.hpp",
  "engine/lighting/include/orbit/lighting/RadianceClipmapResidency.hpp",
  "engine/lighting/include/orbit/lighting/RadianceEstimator.hpp",
  "engine/lighting/include/orbit/lighting/RadianceCacheSampler.hpp",
  "engine/lighting/tests/RadianceClipmapTests.cpp",
  "engine/lighting/tests/RadianceClipmapResidencyTests.cpp",
]
symbols = ["RadianceClipmapConfig", "GpuRadianceCell", "EncodeGpuRadianceCell", "RadianceClipmapMemoryLayout", "RadianceClipmapResidency", "RadianceGpuSnapshot", "RadianceCacheSampler"]
invariants = [
  "The cache is deliberately low-frequency, stable and reusable: first-order SH (one constant term plus X/Y/Z gradients, four RGB coefficients); high-frequency visible-scene detail belongs to the screen-space final gather.",
  "Defaults: baseCellSizeMeters 2.0, levelScale 4.0, levelCount 6, cellsPerAxis 32 (dense logical cube per level); M11 freezes addressing and storage semantics while residency (M12) controls scrolling and physical residency.",
  "GPU cell layout is exactly 96 bytes (static_assert): irradiance0.rgb = L0 with .w = validity [0,1]; irradianceX/Y/Z.rgb = L1 with .w = update age in seconds, sample count and revision (low 24 bits as float); skyIrradiance.rgb = sky-only L0; skyGradient.xyz = direction to the open sky with .w = the one-bounce transport used. Full CPU revision identity stays outside the packed cell.",
  "Sky-only irradiance for normal n is E(n) = l0 * (1 + dot(gradient, n)) with l0 = 0.5 * skyIrradiance * visibleFraction: an unoccluded cell gives the full sky irradiance on an up-facing surface, half on a vertical one and none facing down; terrain and authored proxies occlude it through the cell's sky-visibility traces.",
  "When a source revision change makes an estimated cell stale, the CPU lookup stays strict but the GPU snapshot keeps serving the old irradiance until the refresh lands, so terrain streaming or camera motion never blanks the whole cache or leaves grid-aligned dark slabs while budgeted updates catch up.",
  "Residency marks resident cells for budgeted relighting while preserving their last valid value as the GPU fallback until the refresh reaches them; cell centres are uploaded as camera-relative GPU metres with the cell size in .w.",
  "Authored proxy surfaces (surface class 3) skip the cache fallback entirely (/rendering/lighting/proxy-sun-shadow).",
]
related = ["/rendering/lighting/sky-cache-fill", "/rendering/lighting/visibility-and-reflections", "/rendering/lighting/scheduler"]
verify = [
  "ctest -R Orbit.LightingRadianceClipmap",
  "ctest -R Orbit.LightingRadianceClipmapResidency",
  "ctest -R Orbit.LightingRadianceEstimator",
]
verified = "b0a0de7f"
+++


