# Live CPU performance optimization placement

- Existing owners: `RuntimeSession` owns Vulkan device validation configuration; `StudioTerrainPerformanceDiagnostics` owns the per-viewport diagnostic snapshot.
- Primary insertion points: `engine/runtime/src/RuntimeSession.cpp` for an explicit validation opt-out, `engine/studio_session/src/StudioTerrainPerformanceDiagnostics.cpp` for avoiding unchanged adapter-name reassignment, and `engine/studio_session/src/StudioTerrainPhysicalPageService.cpp` for terrain worker scheduling.
- Canonical state: the Vulkan device's validation configuration and the existing `ViewportRecord` fields.
- Do not duplicate: renderer settings, viewport diagnostics state, or an alternate profiler path.
- Evidence: the live hitch trace sampled `CoreChecks::ValidateDrawVertexBinding` and `vku::concurrent::unordered_map` in Vulkan validation; another sample entered `StudioTerrainPerformanceDiagnostics::RecordViewportStreaming` while freeing a temporary adapter-name string. Live timings averaged 38–49 ms per frame with the Profiler panel closed. During a later 4-second capture, six `TerrainPages` jobs overlapped the window, with one product scope spanning 25.6 seconds; frame average rose to 65 ms. The terrain GPU cache was warm, so this is background physical-page generation rather than repeated render-cache misses.
- Reflection: the runtime environment switch takes effect on the next RuntimeSession/device creation; the worker count is selected when the terrain service is created; the allocation change follows normal native hot iteration.
