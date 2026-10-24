# Orbit CPU scheduling and non-blocking viewport audit (2026-10-24)

Base: `0.1.1-Harmony` at `e96d400b697898b0e2e4f634c576bb0329bfb612`.

## Contract

* Rendering must not synchronously wait for procedural CPU products. Keep the most recently published compatible product, or display a deliberate fallback.
* Generated products capture immutable inputs and have revision/body identity; only finished and current products may publish.
* Do not launch independent `std::async` threads from render-path code. Use `jobs::SubmitBackgroundCpu` (packaged tasks in a shared bounded pool). A legacy `std::async` future must never be destroyed unfinished in a frame path because its destructor can join.
* GPU upload/presentation remains render-owned, bounded, and independently profiled. Background CPU work must never access the graphics command context.

## Repairs in this change

1. First planetary bake: `StudioTerrainBakeController::Tick` requests `StartBake` instead of running `BakeNow` inside the frame.
2. Cloud fields: first-use and refresh builds launch in a background task. A retired viewport future is kept until ready, with bounded admission and an explicit celestial grant abandon.
3. Atmosphere: static LUT and sky-view calculations run on shared bounded background workers (no nested unrestricted parallel-for). A sky request retains immutable shared LUT inputs; completion checks body/fingerprint; stale futures retire without joining and stale scheduler grants are abandoned.
4. Orbital patches: changing terrain source/authority moves pending futures into retired storage before dropping the old state. No synchronous fallback patch sampling when the terrain source lacks shared lifetime.
5. Physical terrain rendering: texel packing, river depth, and lake sampling happen in bounded asynchronous CPU tasks tied to exact immutable page snapshots. GPU buffer publication consumes ready payloads only and maintains existing byte/count caps.

6. Centralized the above CPU jobs (including the terrain sun-shadow builder) through `jobs::BackgroundCpuPool` with configurable `ORBIT_BACKGROUND_CPU_WORKERS` (default approximately hardware threads / 4); Studio prewarms the pool before its first frame.

7. Road/rail corridor cache now retains immutable, shared-ownership geometry snapshots. Dirty corridor derivation, engineering analysis and rail cant planning execute on the bounded background pool. A completed mesh is accepted only when the originating cache binding, dirty token and dependencies still match. In-flight work (including already-discarded jobs) has an eight-job admission cap.
8. Road junction geometry construction, full-scene packet assembly and collision-scene cooking now execute on background workers reading captured immutable path snapshots. The owner thread retains atomic render/contact publication and rejects superseded scene generations. Rapid edits cannot queue unlimited overlapping junction builds.
9. Orbital patch upload processing now has a two-patch-per-compose maximum and a 700-microsecond *between-uploads* budget, with failed preparation/upload isolated from other patches. One individual upload remains non-preemptible.
10. Extended corridor, junction and background job tests for asynchronous completion, stale-work discard and immutable product lifetime.

## Not yet guaranteed by this patch

* `StudioSession::Tick` still resolves path-source centerlines and captures mutable scene inputs synchronously. Although corridor engineering, meshing, junction construction and contact cooking have moved off-thread, source resolution and semantic scene traversal are not yet time-sliced or deadline-bound. Other synchronous Studio updates also remain.
* `GpuFieldGenerator` and `TerrainPreviewRenderer` can still construct/compile shader and driver pipelines inline when terrain identity changes. Shader binaries should be cached/precompiled, with GPU pipeline installation deferred.
* `StudioApplication` still shares its main thread across editor update, viewport composition, command recording, and submission. Frame-slot fences, swapchain resize and presentation may block for GPU/OS reasons.
* Legacy independent pools are still present, and `BackgroundCpuPool` is not yet a process-wide admission/affinity/deadline budget for **all** engine subsystems.
* CPU-to-GPU promotion of completed cloud/atmosphere products is currently bounded by one product per view on completion, not a measured microsecond budget.
* Canceling/clearing an entire terrain bake service can still join its long-running worker during project teardown.

These require dedicated scheduler / snapshot architecture changes and Windows Vulkan stress profiling. This patch is an incremental reduction in render-thread stalls, not a proof of a CPU-independent hard frame deadline.

## Validation

1. Compile `OrbitStudio` with MSVC, warnings-as-errors, on the base's supported Windows environment.
2. Run `Orbit.JobSystemTelemetry`, atmosphere tests, `Orbit.StudioSession` and `Orbit.StudioRenderingIntegration`, then Studio terrain smoke.
3. Profile first planet opening, first cloud generation, sun and altitude animation, source revision invalidation, rapid world swaps, road/rail edits, junction contact generation, and orbital LOD refinement.
4. Record p50/p95/p99/p99.9 frame CPU timings and `vk.fence_wait` separately; inspect Perfetto for `BuildCloudField`, `BuildSkyView`, `BuildStaticLuts`, `BakeNow` and CPU texel packing on `Orbit.Main`.
5. Stress CPU-heavy tasks concurrently and confirm obsolete products never overwrite newer generation state. Test dozens of rapid road edits while a junction worker is running and validate that old collision packets cannot be used for vehicle contacts.
6. GitHub's existing Windows self-hosted workflow does not run on pull requests by design. This draft branch has no completed CI run; run the manual Windows build or dispatch the trusted build workflow explicitly after review.
