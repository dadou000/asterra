+++
path = "/celestial/scheduler"
title = "Celestial work scheduler (budgeted derived work)"
kind = "subsystem"
status = "stable"
summary = "A deterministic frame-budgeted scheduler for derived celestial work (WorkKind: orbital appearance, atmosphere LUT, atmosphere sky view, cloud field, far impostor, ring, aurora and compact-object presentation, on a CPU or GPU backend): requests are keyed per subject and kind, ordered by explicit priority, capped in queue size and granted per frame; completions that no longer match the current authority revision are rejected as stale."
owner_module = "OrbitCelestialScheduler"
keywords = ["scheduler", "budget", "work request", "stale completion", "revision", "priority", "derived work", "queue cap", "grant"]
sources = [
  "engine/celestial_scheduler/include/orbit/celestial_scheduler/CelestialWorkScheduler.hpp",
  "engine/celestial_scheduler/CMakeLists.txt",
]
symbols = ["WorkKey"]
invariants = [
  "Revisions are opaque authority identities compared for exact equality and never numerically ordered.",
  "Enqueue is also the authority update: re-enqueuing the same key with a different revision replaces deferred work and makes any older in-flight completion stale.",
  "A key with an older in-flight generation is not granted again until that generation completes or is rejected, preventing duplicate work explosions.",
  "A completion is accepted only when it still matches current authority; stale completions are consumed and counted but must never publish caches.",
  "Releasing a grant that became irrelevant (for example a viewport retarget) is not a stale completion and publishes nothing.",
  "A hard queue cap prevents a newly viewed system from creating unbounded derived work; the lowest-value deferred requests are discarded first.",
  "Higher priority values are scheduled first; visible work gets a deterministic bias but priority stays explicit so diagnostics and quality policy can tune it.",
]
related = ["/celestial", "/rendering/atmosphere/lut-pipeline", "/rendering/clouds", "/rendering/terrain/invalidation"]
depends_on = ["/foundation/core"]
used_by = ["/editor/studio-ui"]
verify = [
  "ctest -R Orbit.CelestialScheduler",
]
verified = "b0a0de7f"
+++


