+++
path = "/rendering/lighting/eye-adaptation"
title = "Eye adaptation: highlight protection and boost limit"
kind = "concept"
status = "stable"
owner_module = "OrbitPostProcess"
summary = """
Auto-exposure (HumanEyeAdaptation) works in cd/m^2 with one scene-linear unit = 929,563 cd/m^2. Highlight protection (on by \
default) caps exposure so the brightest metered pixel never exceeds the display peak, except glare sources above 1e6 nits; \
the natural boost limit stops photopic gain exceeding the full-daylight setting by more than 6 stops, so dark interiors \
stay dark."""
keywords = ["exposure", "eye adaptation", "auto exposure", "highlight protection", "boost limit", "glare", "tone mapping", "nits", "cd/m2", "display peak", "dark interior"]
sources = [
  "engine/post_process/include/orbit/post_process/HumanEyeAdaptation.hpp",
  "engine/post_process/src/HumanEyeAdaptation.cpp",
  "engine/post_process/include/orbit/post_process/HumanEyeAdaptationHotReload.hpp",
  "engine/studio_ui/src/DisplayEyeRpc.cpp",
  "engine/studio_ui/include/orbit/studio_ui/LightingDisplaySettings.hpp",
  "engine/post_process/tests/HumanEyeAdaptationTests.cpp",
]
symbols = ["kSceneLuminanceNitsPerUnit", "highlightProtection", "glareThresholdNits", "highlightAttackSeconds", "daylightAdaptationNits", "maximumBoostStops", "HumanEyeAdaptationHotReloadInterface"]
invariants = [
  "Exposure math is in cd/m^2; one scene-linear unit = kSceneLuminanceNitsPerUnit = 1361 W/(m^2 sr) x 683 lm/W = 929,563 cd/m^2 (nitsPerSceneUnit).",
  "With highlightProtection on, the brightest metered pixel never exceeds the display peak: exposure is capped so that pixel maps to the tone-mapping peakNits. Reference white and peak are copied from the tone-mapping config every frame - one source of truth.",
  "Sources above glareThresholdNits (default 1e6: the sun disc, glints) are glare and are not protected.",
  "The cap engages fast (highlightAttackSeconds, default 0.04 s) and releases at the normal photopic darkening time.",
  "Photopic gain may exceed the full-daylight setting (daylightAdaptationNits, default 50,000) by at most maximumBoostStops (default 6); this keeps dark interiors from being lifted to mid-grey.",
  "Settings persist in the display settings as display.eye.*; orbit_eye_set validates and changes any subset of fields but does not persist them.",
  "The eye update lives in the hot-reloadable post-process module (interface kHumanEyeAdaptationHotReloadInterfaceVersion = 2); new state fields must default to 'no limit' so running state survives an implementation swap.",
  "The luminance histogram build accumulates per 8x8 group in shared memory and flushes one global atomic per non-empty bin plus the three statistics words per group; per-pixel global atomics on the shared statistics words serialise the dispatch.",
  "The histogram build meters one pixel (the top-left) of every 2x2 quad - a quarter of the reads and atomics - and writes that sample's values into the metering mask for all four pixels, so the overlay still covers the frame; counts are therefore per quad, which the percentile maths (relative weights) does not care about.",
]
related = ["/rendering/lighting"]
depends_on = ["/rendering/lighting"]
verify = [
  "ctest -R Orbit.HumanEyeAdaptation.",
  "orbit_eye_get: brightest pixel in cd/m^2, stops removed by protection, stops the boost limit refused.",
  "Save HumanEyeAdaptation.cpp while Studio runs: the implementation swaps in-process and exposure state is preserved.",
]
verified = "1229ef74"

[[diagnose]]
symptom = "highlights are blown out or the whole scene is too bright"
steps = [
  "orbit_eye_get: check highlight_protection is true and read the brightest pixel (cd/m^2) and the stops removed by protection.",
  "If the brightest pixel is a glare source (sun disc, glint) above glare_threshold_nits it is deliberately unprotected: lower glare_threshold_nits only if non-glare surfaces are being treated as glare.",
  "Compare reference white and peak with the tone-mapping config; they are copied from it every frame.",
]
docs = ["/rendering/lighting"]

[[diagnose]]
symptom = "dark interiors are lifted to mid-grey or look too bright after moving indoors"
steps = [
  "orbit_eye_get: read the stops the boost limit refused; max_boost_stops (default 6) and daylight_adaptation_nits (default 50,000) set the ceiling.",
  "Lower max_boost_stops with orbit_eye_set to keep dark scenes darker; orbit_eye_reset restarts adaptation.",
]
docs = ["/rendering/lighting"]
+++

## Controls

| RPC / MCP | Purpose |
| --- | --- |
| `display.eye_get` / `orbit_eye_get(view_id?)` | config and live state in cd/m^2 |
| `display.eye_set` / `orbit_eye_set(view_id?, highlight_protection?, glare_threshold_nits?, highlight_attack_seconds?, daylight_adaptation_nits?, max_boost_stops?, nits_per_scene_unit?, ...)` | change any subset of fields (validated, not persisted) |
| `display.eye_reset` / `orbit_eye_reset(view_id?)` | restart adaptation |

The Display Diagnostics panel shows the same state.
