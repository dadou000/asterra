+++
path = "/rendering/lighting/scheduler"
title = "Lighting scheduler and stable view"
kind = "subsystem"
status = "stable"
summary = "LightingScheduler turns a per-frame GPU budget and quality policy into a work plan for visibility, GI, reflections and emissive refresh, with asymmetric load response; LightingView keeps stable double-precision semantic coordinates separate from the moving float presentation origin."
owner_module = "OrbitLighting"
keywords = ["lighting scheduler", "budget", "lighting view", "quality", "emissive gi", "overload", "recovery", "work plan", "presentation origin", "stable cell", "ray query preference", "m40"]
sources = [
  "engine/lighting/include/orbit/lighting/LightingScheduler.hpp",
  "engine/lighting/include/orbit/lighting/LightingView.hpp",
  "engine/lighting/tests/LightingSchedulerTests.cpp",
  "engine/lighting/tests/LightingViewTests.cpp",
]
symbols = ["LightingScheduler", "LightingSchedulerConfig", "LightingBudget", "LightingWorkPlan", "LightingView", "StableLightingCell"]
invariants = [
  "Downward response is deliberately faster than upward recovery (overloadResponse 0.70 versus recoveryResponse 0.12): a transient expensive frame sheds optional work immediately and quality returns gradually after pressure disappears.",
  "Each work class has a floor scale (visibility 0.10, GI 0.10, reflections 0.05, emissive 0.10) so load shedding never reaches zero.",
  "Emissive GI quality (emissiveGiQualityScale) is independent of hardware ray-query capability and of the physical emissive authority: it scales only the amount of scheduled emissive refresh work.",
  "Hardware ray query is a backend preference threshold (default 0.35), not a budget: it never raises any count by itself, and disabling it only removes the hardware visibility backend from consideration inside the existing work budget.",
  "Policy layers: Project Settings publishes the project default and Display Diagnostics may publish a session override on top of the authored/default scheduler config; the scheduler stays usable headlessly with no override installed.",
  "Frame protocol: call after the frame slot's fence has completed and before the command list records work for that slot; GPU timings are resolved only after the slot's GPU work is known complete.",
  "LightingView carries stable double-precision semantic coordinates; the presentation origin used to produce float GPU positions may move without changing any stable cache key.",
]
related = ["/rendering/lighting/visibility-and-reflections", "/rendering/lighting/radiance-cache", "/rendering/render-graph", "/editor/viewport"]
verify = [
  "ctest -R Orbit.LightingScheduler",
  "ctest -R Orbit.LightingView",
]
verified = "b0a0de7f"
+++


