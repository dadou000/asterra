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

7. Road/rail corridor cache now retains immutable, shared-ownership geometry snapshots. Dirty corridor derivation, full transport-section engineering and railway cant planning execute on the bounded background pool. A completed mesh is accepted only when the originating cache binding, dirty token and dependencies still match. In-flight work (including already-discarded jobs) has an eight-job admission cap.
8. Road junction geometry construction, full-scene packet assembly, patch mesh preparation and collision-scene cooking now execute on background workers reading captured immutable path snapshots. The owner thread retains atomic render/contact publication and rejects superseded scene generations. Rapid edits cannot queue unlimited overlapping junction builds.
9. Orbital patch upload processing now has a two-patch-per-compose maximum and a 700-microsecond *between-uploads* budget, with failed preparation/upload isolated from other patches. One individual upload remains non-preemptible.
10. Extended corridor, junction and background job tests for asynchronous completion, stale-work discard and immutable product lifetime.
11. Production terrain renderer, analytic GPU field generator and physical page composite now expose device-free shader prewarming requests. Studio submits these to the bounded worker pool before first clipmap construction; DXC's process-wide shader binary cache reuses the compiled products when Vulkan resources are created on the main thread. A failed prewarm is reported without repeatedly compiling every frame. Shader prewarm is covered by the headless DXC terrain shader tests.
12. Cloud per-texel summary reduction moved into the background field build, eliminating a repeated complete cloud-map scan during viewport composition. Completed fields publish with strong staging of the GPU product; a same-body previous time bucket remains eligible for last-ready display during continuous clock advancement.
13. Shader prewarming owns an isolated DXC compiler inside the worker task, avoiding use-after-free when Studio closes while a compile is still queued. The orbital patch builder also refuses borrowed, non-owned terrain authorities rather than dereferencing a null shared owner.

14. Added process-wide, nonblocking CPU admission for the new background workload family. `TrySubmitBackgroundCpu` reserves a slot atomically for both running and queued jobs; low and normal priorities have smaller ceilings so high-priority work retains admission headroom. Terrain, cloud, atmosphere, sun-shadow, shader-prewarm, corridor and junction tasks now defer on rejection and retry later, rather than accumulating an unbounded queue. Includes saturation/priority regression tests and queue telemetry (`BackgroundCpuAdmission`). The legacy `SubmitBackgroundCpu` entry point remains available and is **not** admission-bounded.
15. Cancelled whole-world bake services now retire after cooperative worker completion instead of joining inside Studio's world-close or project-switch frame. Explicit rebake during an in-flight bake coalesces a replacement, cancels the prior worker, and publishes only the newest result. Cancel/retire and deferred restart regressions were added to `Orbit.TerrainBake`.
16. Default geology, tectonic and river bake worker counts now use a share of hardware concurrency rather than all available logical threads. Overrides: `ORBIT_GEOLOGY_BAKE_WORKERS`, `ORBIT_TECTONIC_BAKE_WORKERS`, and `ORBIT_RIVER_BAKE_WORKERS`. Explicit per-bake `workerThreads` options continue to override tectonic/river defaults.

17. Vulkan now creates a shared device-owned `VkPipelineCache`, loads driver- and UUID-compatible data from `%LOCALAPPDATA%/Orbit/PipelineCache`, serializes cache use across graphics/compute pipeline creation and writes the blob at orderly device shutdown. This speeds repeated driver compilation after a warm start but does **not** make cold `vkCreate*Pipelines` nonblocking.
18. Regular Studio rendering now checks the frame-slot timeline through `Fence::CompletedValue` and polls Vulkan swapchain image acquisition with a zero timeout. A busy GPU/frame queue defers the render iteration, leaves the previous frame displayed and returns control to the event loop rather than waiting infinitely. Resize/shutdown synchronization remains explicit.
19. Frame-local road/rail Bézier centerline sampling now runs on CPU workers too; Studio captures endpoint coordinates and reference-body shape into an immutable `PathSourceRequest` instead of sharing mutable frame/body registries. The PathSource regression test checks equivalence with the original evaluator.
20. Persisted planetary bake file loading moved from `TerrainBakeService::Observe` into a deferred background I/O task. Missing/invalid files are resolved before automatic generation starts; explicit rebakes supersede any pending file load.
21. External callers of `JobSystem::Wait` and `WaitIdle` no longer execute arbitrary worker jobs themselves; nested waits on actual job workers retain work-helping behavior. The job telemetry test asserts no execution on an external waiting thread.
22. Windows procedural bake workers run below-normal OS priority while Studio's current foreground editor/render thread runs above-normal priority as a best-effort latency hint. This cannot substitute for independent simulation/editor/render execution threads.

23. `rhi::Device` now exposes bounded nonblocking async graphics/compute pipeline requests that copy all shader bytes and vertex attributes at submission. `VulkanDevice` owns a single lower-priority pipeline compilation thread, publishes finished resources via packaged-task futures, drains the queue before GPU teardown, and reports queue depth/completions/rejections through `PipelineCompilation()`. Rejected jobs retry later without blocking the render thread.
24. Vulkan's cold pipeline worker has its **own** `VkPipelineCache` seeded with the compatible persisted blob; the graphics thread uses a separate device cache. The caches merge at shutdown, avoiding a common mutex being held across multi-second driver compilations and blocking unrelated render work.
25. Production terrain's graphics and water pipelines and its analytic compute, physical-page composite and hydrology region-delta compute pipelines are now all requested through the driver worker. Each user zero-time polls the corresponding future and refrains from recording those draws/dispatches until all required pipeline products are published. The water renderer does not draw over a missing terrain pipeline.
26. Infrastructure scene discovery is now resumable with a semantic-revision-aware cursor and an explicit 96-objects-per-tick processing quota; modifying the world restarts discovery without applying obsolete results. Individual SQLite Children() fetches and scene-side junction scans still need their own latency budgets.
27. The interactive Vulkan terrain smoke now waits for an actual terrain draw, not two arbitrary frames. The trusted optional GPU workflow runs cold and warm smoke and verifies a persisted Vulkan driver cache blob, with a 90-second first-draw limit. No run results were observed from this editing environment.

## Not yet guaranteed by this patch

* Frame-local road and rail centerline sampling, engineering, meshing, and junction cooking are off-thread. Studio still performs semantic scene traversal, path endpoint/shape capture, and other mutable session updates on the main thread; these operations are not time-sliced or deadline-bound.
* The production terrain graphics/compute pipeline set is now compiled asynchronously on a dedicated Vulkan-owned worker with a separate cache, on top of the earlier background DXC prewarm. **Other engine renderers** can still invoke synchronous `CreateGraphicsPipeline` / `CreateComputePipeline` and all Vulkan texture/buffer allocation is still synchronous. In particular, `VulkanDevice::CreateTexture` uses `TransitionNewImageBlocking` (including `vkQueueWaitIdle`) to initialize texture layouts; safely removing that wait requires a coordinated lazy-first-use layout transition for all relevant attachment, copy and sampled/storage-texture access paths.
* The editor/update/viewport/submit path remains on one foreground thread. Normal GPU frame-slot waits and image acquisition are nonblocking/deferrable, and terrain pipeline creation is asynchronous. Swapchain resize, device destruction, synchronous resource allocation/first-layout transitions, other renderers' pipeline creation and some `vkQueuePresentKHR` implementations can still block for GPU/OS reasons.
* The new bounded admission protects the migrated background CPU workload family, but legacy independent pools and the compatibility `SubmitBackgroundCpu` path still lack process-wide admission, CPU affinity and true deadline enforcement.
* CPU-to-GPU promotion of completed cloud/atmosphere products is currently bounded by one product per view on completion, not a measured microsecond budget.
* Whole-world bake cleanup and overlapping explicit bake requests defer long joins; persisted planet bake loads are asynchronous. Final application shutdown still joins remaining workers by design, and the number of simultaneously retired services is not yet globally budgeted.

These require dedicated scheduler / snapshot architecture changes and Windows Vulkan stress profiling. This patch is an incremental reduction in render-thread stalls, not a proof of a CPU-independent hard frame deadline.

## Validation

1. Compile `OrbitStudio` with MSVC, warnings-as-errors, on the base's supported Windows environment.
2. Run `Orbit.JobSystemTelemetry`, `Orbit.PathSource`, `Orbit.TerrainBake`, `Orbit.TerrainSurfaceShader`, `Orbit.TerrainGpuPhysicalPageCompositeShader`, atmosphere tests, `Orbit.StudioSession` and `Orbit.StudioRenderingIntegration`, then Studio terrain smoke.
3. Profile first planet opening, first cloud generation, sun and altitude animation, source revision invalidation, rapid world swaps, road/rail edits, junction contact generation, and orbital LOD refinement. Compare first/cold versus second/warm Vulkan pipeline cache launches and saturate the FIFO swapchain.
4. Record p50/p95/p99/p99.9 frame CPU timings, missed frame-slot/image-acquire opportunities and `vk.fence_wait` separately; inspect Perfetto for `BuildCloudField`, `BuildSkyView`, `BuildStaticLuts`, `BakeNow` and CPU texel packing on `Orbit.Main`.
5. Stress CPU-heavy tasks concurrently and confirm obsolete products never overwrite newer generation state. Test dozens of rapid road edits while a junction worker is running and validate that old collision packets cannot be used for vehicle contacts.
6. GitHub's existing Windows self-hosted workflow does not run on pull requests by design. This draft branch has no completed CI run; run the manual Windows build or dispatch the trusted build workflow explicitly after review.

## Review notes

- Source review completed for the changed corridor cache, road-junction pipeline, atmosphere renderer, job scheduler and orbital patch uploader; brace and C++ comment/string lexical balance checks passed for the main modified source files. This is **not equivalent to compilation**.
- The existing self-hosted Windows workflow `.github/workflows/orbit-self-hosted-unpacked.yml` accepts trusted manual dispatch but intentionally has no pull-request trigger. There were no completed workflow runs for this draft head during this pass.
- `PlanInfrastructureSection` used to call `BuildPathDerived` synchronously inside the corridor resolver. It is now within the async job, where it can spend unbounded time without extending `StudioSession::Tick`.
- Terrain DXC prewarming and the dedicated Vulkan driver pipeline worker now avoid ordinary main-thread pipeline construction for production terrain. GPU allocation and texture layout initialization can still be non-preemptible render-thread operations; the 700-microsecond inter-upload limit is not a hard deadline on an individual operation. Validate the graphics, water, analytic, physical-page and hydrology pipeline publication sequence under real-device validation.

## Background admission contract

- `TrySubmitBackgroundCpu` is a **non-waiting** request, returning `std::nullopt` when priority capacity is exhausted. Callers must keep last-ready products and retry rather than treating backpressure as simulation failure.
- Running and queued work consume admission until their packaged task invocation completes. Abandoned future ownership does not free admission early; a completed worker does.
- `BackgroundCpuAdmission().outstanding/rejected/capacity` records aggregate pressure. Admission headroom is not a substitute for OS-level CPU reservations or frame deadlines.
- CPU worker cancellation remains cooperative for long jobs. No hard real-time FPS guarantee follows without a separate frame-submission thread and GPU synchronization budget.
