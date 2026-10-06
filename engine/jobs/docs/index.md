+++
path = "/foundation/jobs"
title = "Job system"
kind = "subsystem"
status = "stable"
summary = "JobSystem runs prioritised jobs on named worker pools with JobGroup completion tracking and lock-free telemetry. A pool's worker count defaults to one per hardware thread minus one, background pools take a share of the machine, and an environment variable can override the computed count."
owner_module = "OrbitJobs"
keywords = ["jobs", "job system", "thread pool", "workers", "priority", "job group", "telemetry", "background pool", "async"]
sources = [
  "engine/jobs/include/orbit/jobs/JobSystem.hpp",
  "engine/jobs/CMakeLists.txt",
]
symbols = ["JobSystemTelemetry"]
invariants = [
  "Engine work that is expensive (generation, streaming, compilation, simulation) must be expressible as jobs; APIs must not silently require the main thread (/rules/architecture).",
  "workerCount 0 means one worker per hardware thread minus one; several pools share one process, so a background pool takes hardware_threads / divisor (at least a minimum) unless the environment override is a positive integer.",
  "Worker threads are named 'Orbit.<name>.<index>' so they can be told apart in a debugger or profiler.",
  "JobSystemTelemetry is a lock-free approximate snapshot for UI/profiling: `outstanding` includes queued plus running work and `queued` may differ by one while a worker transitions.",
]
related = ["/world/procedural-graph", "/rules/architecture"]
depends_on = ["/foundation/core"]
used_by = ["/world/path-routing", "/world/procedural-graph"]
verify = [
  "ctest -R Orbit.JobSystem",
  "ctest -R Orbit.JobSystemTelemetry",
]
verified = "b0a0de7f"
+++

`ProceduralGraph` schedules its builds on this system (`/world/procedural-graph`).
