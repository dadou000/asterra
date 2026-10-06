+++
path = "/rendering/lighting/visibility-and-reflections"
title = "Visibility queries, final gather and reflection policy"
kind = "subsystem"
status = "stable"
summary = "One provider-agnostic visibility API (callers state what the answer must guarantee, never which backend) over screen-space, analytic, terrain-heightfield, software-proxy and hardware ray-query providers, the screen-space final gather, and the reflection policy that decides per pixel between the screen hit, the radiance cache and exact rays."
owner_module = "OrbitLighting"
keywords = ["visibility", "visibility query", "provider", "ray query", "screen space", "terminal miss", "final gather", "reflection policy", "roughness", "mirror", "glossy", "exact reflection", "fallback", "nearest surface"]
sources = [
  "engine/lighting/include/orbit/lighting/Visibility.hpp",
  "engine/lighting/include/orbit/lighting/ReflectionPolicy.hpp",
  "engine/lighting/include/orbit/lighting/ScreenSpaceFinalGather.hpp",
  "engine/lighting/include/orbit/lighting/HybridReflectionRenderer.hpp",
  "engine/lighting/include/orbit/lighting/ExactReflectionQueryRenderer.hpp",
  "engine/lighting/tests/VisibilityTests.cpp",
  "engine/lighting/tests/ReflectionPolicyTests.cpp",
]
symbols = ["VisibilityRegistry", "VisibilityProvider", "VisibilityRequirements", "VisibilityQuery", "PlanReflection", "ReflectionPolicyConfig", "ReflectionFallback", "ScreenSpaceFinalGatherRenderer", "HybridReflectionRenderer", "ExactReflectionQueryRenderer"]
invariants = [
  "Callers state requirements (offscreen coverage, exact geometry, planetary range, surface material, maximum nominal error, minimum confidence) and never name a backend; capabilities are a bit set (ViewDependent, Offscreen, ExactGeometry, PlanetaryRange, DynamicGeometry, SurfaceMaterial) and backend kind is only a selection hint - a provider may still report lower per-query confidence or an unresolved result.",
  "A miss is terminal only when the provider can PROVE that no acceptable hit exists for the query domain: a screen-space miss is never terminal, so the query continues to a provider with off-screen coverage.",
  "Occlusion/nearest-surface mode evaluates every qualifying provider and returns the closest sufficiently confident hit; unlike Trace(), a terminal miss from one provider does not suppress a hit from another representation.",
  "Batch GPU providers operate in the active LightingView frame (camera-relative origin and normalised direction); arbitrary-frame and double-precision transforms happen before encoding.",
  "PlanReflection order: a screen hit whose confidence is at least 0.70 is used outright (rough or mirror alike); only otherwise do the roughness rules apply. Non-finite inputs are sanitised first (roughness -> 1, confidence -> 0, importance -> 0).",
  "Reflection plan (PlanReflection) by roughness, screen confidence, importance and capability: mirror-like surfaces (roughness up to 0.08) request precise off-screen continuation when the screen trace cannot answer confidently (confidence below 0.70); glossy surfaces (up to 0.45) still use screen traces but degrade to the broad radiance-cache representation instead of spending exact rays; exact visibility needs importance of at least 0.20.",
  "The exact-ray fallback is hardware ray query when available, else software exact, else the radiance cache (ReflectionFallback); disabling hardware ray query never selects a different lighting model, it only removes one backend from consideration.",
  "The screen-space final gather can write its coverage (confidence, brightness, magenta = nothing returned) instead of adding to scene colour, for diagnosis (indirect_coverage_view).",
]
related = ["/rendering/lighting/radiance-cache", "/rendering/lighting/scheduler", "/rendering/rhi", "/rendering/terrain/clipmaps/debugging"]
verify = [
  "ctest -R Orbit.LightingVisibility",
  "ctest -R Orbit.LightingReflectionPolicy",
  "ctest -R Orbit.LightingScreenSpaceFinalGather",
  "ctest -R Orbit.LightingScreenSpaceVisibility",
  "ctest -R Orbit.LightingSoftwareProxyVisibility",
]
verified = "b0a0de7f"
+++


