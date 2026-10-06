+++
path = "/rendering/lighting/sky-cache-fill"
title = "Sky-only radiance cache channel"
kind = "concept"
status = "stable"
owner_module = "OrbitLighting"
summary = """
Each radiance cache cell (96 bytes) carries a sky-only irradiance next to its one-bounce L1. The estimator fills it with \
the atmosphere's sky irradiance times the open fraction of the cell's hemisphere, traced against terrain, authored proxies \
and analytic bodies. DirectLightingRenderer adds albedo/pi times E(n) on every near-field surface, so cast shadows and \
enclosed spaces are sky-lit instead of black."""
keywords = ["sky fill", "radiance cache", "sky irradiance", "enclosed", "shadow black", "l0", "gradient", "estimate radiance cell", "bypass_sky_cache", "ambient"]
sources = [
  "engine/lighting/include/orbit/lighting/RadianceEstimator.hpp",
  "engine/lighting/src/RadianceEstimator.cpp",
  "engine/lighting/include/orbit/lighting/RadianceClipmap.hpp",
  "engine/lighting/src/DirectLighting.cpp",
  "engine/lighting/tests/RadianceEstimatorTests.cpp",
]
symbols = ["EstimateRadianceCellWithSky", "GpuRadianceCell", "DirectLightingRenderer"]
invariants = [
  "GpuRadianceCell is exactly 96 bytes (static_assert): skyIrradiance.rgb = L0, skyGradient.xyz = direction toward the open sky, skyGradient.w = the one-bounce transport the L1 used.",
  "The sky channel is not scaled by the 0.18 one-bounce transport that the old L1 sky used.",
  "The fill is albedo / pi * E(n) with E(n) = l0 * (1 + dot(gradient, n)), applied on every near-field surface independent of how confident the screen-space gather is.",
  "The fill is added to scene colour after the gather has read it and never into the gather's history, so it cannot accumulate frame to frame.",
  "Authored proxy surfaces keep their ray-traced sky fill instead of the cell fill (more accurate); they skip the cache entirely.",
  "Reflections are unchanged: the hybrid and exact reflection shaders rebuild the legacy one-bounce sky from the new channel (2 * l0 * transport, old lobe weights); the cache fallback no longer carries a sky.",
  "bypass_sky_cache only disables the fill; the cache keeps estimating the channel.",
  "The fill is zero without sunlight (night side, space) and whenever the near-field indirect stack is off (planet seen from orbit, bypass_indirect_lighting).",
  "A cell layout change needs no content migration: a new generation starts with an empty cache that refills.",
]
related = ["/rendering/lighting/proxy-sun-shadow"]
depends_on = ["/rendering/lighting"]
verify = [
  "ctest -R Orbit.LightingRadianceEstimator.",
  "orbit_view_terrain_layers_set(bypass_sky_cache=true): shadowed and enclosed terrain areas go dark (sky fill off); back to false restores them.",
]
verified = "b0a0de7f"

[[diagnose]]
symptom = "cast shadows or enclosed spaces are black instead of sky-lit"
steps = [
  "orbit_view_terrain_layers_get: bypass_sky_cache and bypass_indirect_lighting must be false; confirm the view is near-field (not orbit) and the body is sunlit (the sky summary is zero at night).",
  "orbit_view_terrain_layers_set(indirect_coverage_view=true): magenta pixels got nothing from the gather; the sky fill should still apply there because it does not depend on gather confidence.",
  "If the fill is missing only on authored proxy surfaces, that path is the ray-traced proxy sky fill (see /rendering/lighting/proxy-sun-shadow), not the cache.",
]
docs = ["/rendering/lighting"]

[[diagnose]]
symptom = "sky fill appears to accumulate, smear or ghost over time"
steps = [
  "The fill must be added after the gather read the cache and never written into the gather's history; check that ordering in DirectLightingRenderer first.",
  "Compare with bypass_sky_cache=true: the ghosting must vanish with the fill off.",
]
docs = ["/rendering/lighting"]
+++

## Where it is computed and consumed

`EstimateRadianceCellWithSky` (`RadianceEstimator`) writes the channel per cell, tracing the open fraction of the
cell's hemisphere against everything the visibility registry knows. `DirectLightingRenderer` reads the cache and adds
the fill; reflections reconstruct the legacy one-bounce sky from the new channel so their look is unchanged.

## Hot iteration

The estimator, cell layout and shaders are lighting-library sources (automatic Studio-generation handoff). No content
migration is needed.
