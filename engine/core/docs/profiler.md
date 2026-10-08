+++
path = "/editor/profiler"
title = "CPU micro-profiler, hitch capture and GPU pass timing"
kind = "subsystem"
status = "stable"
owner_module = "OrbitCore"
summary = """
Always-on lock-free scope profiler for the Studio process: per-thread ring buffers, a CPU-core view, automatic hitch \
captures (the last few seconds written as a Perfetto/Chrome trace when a frame exceeds the threshold), a watchdog that \
samples the stalled main thread's stack, GPU per-render-graph-pass timing, the Profiler panel and eight RPC/MCP methods. \
Start with orbit_profiler_status after any freeze."""
keywords = ["profiler", "hitch", "freeze", "stall", "perfetto", "trace", "scope", "watchdog", "stack sample", "gpu passes", "calibrated timestamps", "frame time", "profile scope", "chrome tracing"]
sources = [
  "docs/ORBIT_PROFILER.md",
  "engine/core/include/orbit/profiler/Profiler.hpp",
  "engine/core/src/Profiler.cpp",
  "engine/editor_rpc/src/ProfilerRpc.cpp",
  "engine/studio_ui/src/ProfilerPanelRpc.cpp",
  "engine/studio_ui/src/ProfilerModel.cpp",
  "engine/studio_ui/src/ProfilerUi.cpp",
  "engine/editor_ui/src/EditorUi.cpp",
  "apps/editor/src/StudioApplication.cpp",
  "engine/render_graph/src/GpuPassTimer.cpp",
]
symbols = ["ORBIT_PROFILE_SCOPE", "Intern", "StartWatchdog", "BeginFrame", "EndFrame", "CancelFrame", "GpuPassTimer", "ProfilerModel"]
applies_to = ["engine/core/src/Profiler.cpp", "engine/core/include/orbit/profiler/**", "engine/editor_rpc/src/ProfilerRpc.cpp", "engine/studio_ui/src/Profiler*"]
invariants = [
  "Recording a scope takes no lock and allocates nothing: each thread owns a ring of 16384 events of 32 bytes (static_assert on the event size), overwritten when full, so memory is fixed.",
  "Scope names must be string literals or profiler::Intern(...) pointers; a dynamic std::string pointer would dangle in the ring.",
  "ORBIT_PROFILE_SCOPE(\"name\") is the only thing instrumenting code needs; JobSystem already wraps every job, and shader compilation, pipeline/buffer/texture creation, queue submit, fence wait, swapchain acquire/present, terrain streaming, patch build/upload, physical page products and atmosphere LUT builds are instrumented. New CPU-heavy work gets a scope.",
  "BeginFrame()/EndFrame() bracket each Studio loop iteration after the intentional idle-pacing wait; CancelFrame() is used when rendering is skipped (minimised window), so idle waits never count as hitches.",
  "Defaults: hitch_threshold_ms 100, stall_threshold_ms 250 (then the main thread's stack is sampled about every 50 ms), capture_window_ms 4000, max_hitch_files 24 (older hitch files are deleted). profiler.configure changes them live; enabled=false makes a scope one relaxed load.",
  "Stall stacks come from a watchdog thread that briefly suspends the main thread, unwinds with RtlVirtualUnwind and symbolises on the watchdog thread with a private dbghelp session (so another SymInitialize in the process cannot disable symbols). Symbols need the PDB beside the exe (ORBIT_PROFILER_SYMBOLS, default ON; changing it reconfigures CMake and rebuilds every object once).",
  "GPU spans are bottom-of-pipe timestamps written after each render-graph pass (RenderGraph::Execute(commands, &gpuPassTimer)); the gap between neighbours is the time the GPU took to finish that pass, including time stuck behind earlier work. They are read after the frame slot's fence completes, so they appear two to three frames later. Passes under 50 us stay off the timeline (GpuPassTimer::LastFrame keeps every pass); the core view omits slices under 30 us.",
  "GPU work submitted outside the render graph (the lighting and surface-volume recorders keep their own timings) is folded into whichever pass it runs next to; do not read a pass time as exclusive of that.",
  "With VK_KHR_calibrated_timestamps GPU spans line up exactly with CPU lanes; without it they keep their durations but are anchored to when the frame was resolved (GpuPassTimer::Calibrated() tells which).",
  "The ProfilerModel owns viewport-only capture defaults (4 seconds and 1440p); its duration and resolution preset controls are shared with profiler.panel_get/set and MCP. Capture viewport only invokes the same StudioApplication-owned timed action as profiler.viewport_capture, skips non-viewport panels during the sample, keeps the primary viewport live at the selected target size, writes a trace and restores the shell and ordinary target size. profiler.viewport_capture_status returns progress, preset dimensions and the resulting path.",
  "Profiler code lives in OrbitCore: editing it takes the ordinary automatic generation handoff and a failed hot build leaves the running ring buffers and watchdog untouched (/rules/hot-iteration).",
]
related = ["/editor/studio-ui", "/foundation/core", "/rendering/terrain/clipmaps/rebuild-hitches", "/legacy/orbit-profiler"]
depends_on = ["/foundation/core", "/editor/mcp-rpc"]
used_by = ["/rendering/terrain/clipmaps/rebuild-hitches", "/rendering/terrain/clipmaps/debugging"]
verify = [
  "ctest -R Orbit.Profiler and Orbit.ProfilerModel.",
  "Provoke a hitch and call orbit_profiler_status: a hitch file with frame_ms and top_stack_frames must appear.",
  "python -m py_compile tools/mcp_server/orbit_editor_mcp_server.py after touching the MCP tools.",
]
verified = "55d48117"

[routes]
"multi-second freeze, what was the main thread doing" = "/legacy/orbit-profiler"
"which GPU pass is slow" = "/rendering/terrain/clipmaps/debugging"

[[diagnose]]
symptom = "Studio froze or hitched and I need to know why"
steps = [
  "orbit_profiler_status (profiler.status): lists hitch captures with frame_ms and top_stack_frames, the functions most often at the top of the stalled main-thread stack.",
  "Open the hitch file's path in ui.perfetto.dev or chrome://tracing. Files are hitch-YYYYMMDD-HHMMSS-f<frame>-<ms>ms.json in %LOCALAPPDATA%\\Orbit\\profiler (override ORBIT_PROFILER_DIR).",
  "Look at the main-thread lane for the long slice and the 'STALLED in ...' instants (args.stack is the symbolised call stack). A long slice inside dxcompiler.dll or a driver call is a rebuild or compile on the frame thread (/rendering/terrain/clipmaps/rebuild-hitches).",
  "If stacks have no function names the PDB is missing: build with ORBIT_PROFILER_SYMBOLS on.",
]
docs = ["/rendering/terrain/clipmaps/rebuild-hitches"]

[[diagnose]]
symptom = "need the last few seconds without waiting for a hitch"
steps = [
  "orbit_profiler_capture (profiler.capture) writes the last capture_window_ms of everything.",
  "Or pause the panel (orbit_profiler_panel_set paused=true) then orbit_profiler_snapshot(top_scopes, slowest) for heaviest scopes, longest slices, top_gpu_passes and stall samples as data.",
]
docs = ["/editor/studio-ui"]
+++

## Tools

| MCP tool | RPC method | Notes |
| --- | --- | --- |
| `orbit_profiler_status` | `profiler.status` | hitch list, current settings |
| `orbit_profiler_configure` | `profiler.configure` | `enabled`, `hitch_threshold_ms`, `stall_threshold_ms`, `capture_window_ms`, `max_hitch_files` |
| `orbit_profiler_capture` | `profiler.capture` | manual capture of the last window |
| `orbit_profiler_panel_get` / `_set` | `profiler.panel_get` / `profiler.panel_set` | panel state: paused, history, hide-shorter-than, by-core, highlight |
| `orbit_profiler_snapshot` | `profiler.snapshot` | analysis of the visible range as data (`top_gpu_passes` included) |

`profiler.status`, `configure` and `capture` are registered in `engine/editor_rpc/src/ProfilerRpc.cpp`; the panel methods in
`engine/studio_ui/src/ProfilerPanelRpc.cpp`.

## Reading a trace

- `Orbit.*` threads: every scope on that thread, nested; worker pools are `Orbit.<Pool>.<n>` (for example `Orbit.TerrainPages.3`),
  each job a `<Pool>.job` slice.
- Process "CPU cores": one lane per logical core holding the same slices on the core they started on (`args.core`).
- Synthetic lanes: `Frames`, `Main loop phases`, `Main loop sub-phases` (the `studio.cpu_timings` phases), `Compose stages`,
  `GPU passes` and `GPU frames`.

The long-form walk-through of the panel (frame strip, pause on hitch, inspect hitch files) is `docs/ORBIT_PROFILER.md`
(`/legacy/orbit-profiler`).
