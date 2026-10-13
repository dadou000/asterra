+++
path = "/editor/studio-ui/debug-center"
title = "Debug workspace: canonical entry point for tests and diagnostics"
kind = "subsystem"
status = "experimental"
summary = "Orbit Studio's Debug workspace is the default UI location for finding, running and inspecting engine diagnostics. Existing test owners, report/capture services, and RPC/MCP commands remain authoritative; the Debug panel links to them instead of cloning implementations."
owner_module = "OrbitStudioUi"
keywords = ["debug", "debugging", "test", "tests", "validation", "experiments", "diagnostics", "terrain", "construction", "lighting", "profiler", "regression"]
sources = [
  "engine/studio_ui/include/orbit/studio_ui/StudioShellModel.hpp",
  "engine/studio_ui/src/StudioViewportPanels.cpp",
  "engine/studio_ui/include/orbit/studio_ui/DebugViewUi.hpp",
  "engine/studio_ui/src/DebugViewUi.cpp",
  "engine/studio_ui/include/orbit/studio_ui/DebugTestRegistry.hpp",
  "engine/studio_ui/src/DebugTestRegistry.cpp",
  "apps/editor/src/StudioApplication.cpp",
  "engine/studio_session/include/orbit/studio_session/StudioTerrainPhysicalPageService.hpp"
]
symbols = ["StudioWorkspaceMode", "StudioViewportPanels::DrawWorkspaceBand", "DebugViewUi", "DebugTestRegistry", "RegisterBuiltInDebugTests", "StudioTerrainPhysicalPageSnapshot::SampleSurfaceHeight"]
invariants = [
  "The top-level Debug workspace and its registered Debug panel are the canonical user-facing location for engine diagnostics, tests and experiments.",
  "A test lives with the subsystem that owns the behavior; Debug presents and invokes it, not a second copy of the implementation.",
  "A visible PASS must describe exactly what was checked; it cannot imply GPU visual validation, collision equivalence or scenario execution when these were not measured.",
  "Expensive tests run on demand, not on every panel redraw. Read-only checks do not mutate the user's authored project.",
  "Existing debugging RPC/MCP commands, performance reports, screenshots and subsystem tools remain usable through their native interfaces.",
  "The editor stays a single Orbit.exe application; no parallel diagnostic launcher is created."
]
+++

## Where to debug

Open **Orbit Studio → Debug** in the main workspace tabs. This activates the **Debug / Test Center** while keeping the viewport accessible. The workspace can also be selected programmatically using `Debug`. The older **Debug** panel for G-buffer controls is reused; it has not been duplicated.

The Debug center groups existing tools into:
- **Terrain / Infrastructure:** Surface Authoring, Body Map / Debug View, and the construction terrain sanity check.
- **Lighting / Atmosphere:** Display Diagnostics, Shading and Celestial.
- **Performance / Reports:** Profiler and Reports, when those panels are registered.
- **Viewport GBuffer:** Lit, Albedo / Roughness, Normal / Metallic, Emission / Meta.

Some destination panels are optional or registered only by certain Studio configurations; a missing panel is reported rather than treated as a successful operation.

## Central debug test registry

`DebugTestRegistry` is owned by the active `DebugViewUi` instance. Each descriptor supplies a unique stable ID, label, category and synchronous on-demand `run(StudioSession&)` callback. The registry returns one of **NOT RUN**, **PASS**, **FAIL**, **NOT READY**, remembers the last outcome, and isolates standard exceptions so the Debug panel can report failures without escaping the UI callback.

The initial `terrain.physical_page_sanity` test is registered by `RegisterBuiltInDebugTests`, not implemented inside the drawing function. To add an experiment, register a descriptor with the registry (or a future plugin-facing registration adapter); do not append a hardcoded Run button to `DebugViewUi::Draw`. The registry keeps up to 128 in-memory run records globally, each with a sequence number, elapsed wall-clock time, world generation and outcome. The Debug panel shows the latest timing and per-test history count. History is not persisted between editor sessions. The current interface does not yet provide asynchronous execution, screenshot capture or RPC registration of individual tests.

## Construction Terrain Validation (first runnable check)

In **Debug → Terrain / Infrastructure**, click **Run** next to **Terrain / Construction terrain / physical-page sanity**. The check reads the primary viewport's current observer physical page from `StudioSession::TerrainRuntime()`; it does not construct a road or modify the world.

Expected outcomes:
- **NOT READY:** no primary terrain viewport or no currently published material page/status.
- **FAIL:** published revision fingerprint is zero or differs from the page rebuild status, or the sampled center elevation is unavailable/non-finite.
- **PASS:** the material page is present, published and current revisions match, and one bilinear center-height query is finite.

**PASS is not proof of rendered road grading.** It does not compare the CPU surface against GPU readback, scan the entire page, test seam continuity, exercise collision, inspect upload latency, or capture a screenshot. Do not label this a construction end-to-end test.

## Adding a new experiment

1. Identify the owning subsystem and define the test's inputs, expected result, failure taxonomy and cleanup. Keep reusable validation logic outside the UI.
2. Register one Debug navigation/run entry; group it under Terrain, Infrastructure, Lighting, Weather, Physics, Performance or Regression. Reuse existing registered test commands and capture/report services.
3. If automation is required, expose the **same** operation through RPC/MCP. Never simulate UI clicks to drive an engine test.
4. Display **Not run / Running / Pass / Fail / Not ready** distinctly. Include tested build/capture identity, input parameters, pertinent page/job revisions, runtime errors, and artifacts when available.
5. Tests that create or modify scene content must use a reversible, isolated fixture or an explicit user-authorized operation. Clean up temporary data.
6. Record verification steps and any limits in this document and the owning subsystem's doc block.

## Reference migration rule

Older docs and code may describe debugging inside Planet tools, Lighting experiments, Body Map or individual specialist panels. Those remain **implementation/authoring surfaces**, but all user-facing instructions about *where to find tests* should begin with **Debug** and then direct to the specialized capability. Existing scenario commands can remain in Explorer until registered in the central Debug directory; do not claim they have moved.

## Verification

1. Build Orbit Studio and confirm the eighth top-level workspace tab is **Debug**.
2. Switch via the tab and programmatic workspace command; confirm that the Debug panel and existing viewport are visible.
3. Check that group shortcuts open registered tools and show a clear message for missing tools.
4. With no physical page ready, run construction validation and observe **NOT READY**. With a ready page, verify revision and height results against `PageStatus` and `SampleSurfaceHeight`.
5. Run `python tools/orbit_docs_cli.py check` and the Studio test suite.

**Validation status:** Source wiring exists on Harmony, but neither a local build nor interactive verification of this workspace is recorded here.

## Report capture controls

Open **Debug → Reports** and select a report. The **Capture tools** section offers:

- **Export capture**: for a performance report with a recorded JSON trace, returns the existing trace in `Captures`; otherwise exports the existing screenshot PNG. It does not generate a new capture.
- **Open capture folder**: creates (if absent) and opens the same `Captures` directory through the platform's existing native file-browser hook.
- **Capture performance trace from condition**: enabled for performance-scoped reports. Restores the saved starting condition (simulation time paused and camera pose/target), waits for the target and eight settling ticks, collects approximately two seconds of live profiler events, then writes `Captures/report-<id>-performance.json` through the existing `profiler::Capture` implementation. This is a Chrome/Perfetto trace, **not a screenshot**. Capture fails if the recorded condition cannot be restored. Recorded timing may differ if the paused simulation suppresses the workload; capturing the original dynamic workload requires a separate replay policy.

Screenshot captures and native folder opening reuse the editor hooks in `apps/editor/src/StudioApplication.cpp`; performance traces reuse the existing `profiler::Capture` mechanism. No second profiler or screenshot recorder is introduced. The trace location is deterministic per report ID and a new capture replaces the previous trace for that report.
