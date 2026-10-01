# Orbit CPU micro-profiler

An always-on profiler for the Studio process. It answers two questions:

- **What was every thread and CPU core doing?** One lane per thread, plus one lane per CPU core, with every job.
- **What happened during that multi-second freeze?** A hitch (a frame longer than the threshold) writes the last few
  seconds of everything to disk automatically, and a watchdog samples the main thread's call stack *while it is
  stuck*, so a hang inside code nobody instrumented still points at a function.

Code: `engine/core/include/orbit/profiler/Profiler.hpp`, `engine/core/src/Profiler.cpp` (tests: `Orbit.Profiler`);
panel: `engine/studio_ui/src/ProfilerUi.cpp`, state in `ProfilerModel.cpp` (tests: `Orbit.ProfilerModel`).

## Using it

Nothing to start: it runs from the first frame. After a freeze:

1. `orbit_profiler_status` (RPC `profiler.status`) lists the hitch captures with `frame_ms` and
   `top_stack_frames` — the functions most often at the top of the stalled main thread's stack.
2. Open the hitch's `path` in <https://ui.perfetto.dev> (or `chrome://tracing`).

For a manual capture of the last few seconds: `orbit_profiler_capture` (RPC `profiler.capture`).

Files go to `%LOCALAPPDATA%\Orbit\profiler` (override with `ORBIT_PROFILER_DIR`). Hitch files are named
`hitch-YYYYMMDD-HHMMSS-f<frame>-<ms>ms.json`; only the newest `max_hitch_files` (24) are kept.

### The Profiler panel

Open **View > Profiler** (or `orbit_panel_focus("Profiler")`). It shows the same data as a trace file without
leaving Studio:

- **Frame strip** at the top: the recent frames as bars (green, amber, red above the hitch threshold). The
  outlined box is what the timeline below is showing. Click a bar to zoom to that frame; drag to slide the view.
- **Timeline**: one lane per thread (scopes nested by call depth) or, with **By core**, one lane per CPU core.
  Synthetic lanes (`Frames`, `Main loop phases`, `Compose stages`) come first. Scroll to zoom around the pointer,
  drag to pan, click a slice to select it, double-click a slice to zoom to it and double-click empty space to reset.
  Hover for name, thread, core, start, duration and self time. Red ticks are stack samples from a stall; hover one
  for its stack.
- **Pause / Resume live**: live mode refreshes about four times a second and scrolls. Pausing freezes a snapshot of
  the last *History* seconds. Zooming, panning or selecting a slice pauses automatically.
- **Pause on hitch** (on by default): when a frame exceeds the hitch threshold the panel pauses itself already
  framed on the long frame, so a freeze you did not see happen is waiting for you.
- **Hide slices shorter than** and **Highlight scope** (dims everything else) cut the noise.
- **Selected slice**: scope, thread, core, start, duration, self time and nested scope count.
- **Heaviest scopes in view** and **Longest slices in view**: the analysis over the visible range; click one to
  highlight it or jump to it.
- **Stalled stack samples**: which functions the main thread sat in during a stall.
- **Recorded hitches**: each hitch file with an **Inspect** button that loads it into the panel (paused, read from
  disk on a worker thread).
- **Write trace file** saves the current history for Perfetto.

The panel only does work while it is open. The same controls exist over RPC/MCP: `profiler.panel_get`,
`profiler.panel_set` and `profiler.snapshot`.

### Reading a trace

- **`Orbit.*` threads** (process "Orbit"): every scope on that thread, nested. Worker pools appear as
  `Orbit.<Pool>.<n>` (for example `Orbit.TerrainPages.3`) and each job is a `<Pool>.job` slice.
- **Process "CPU cores"**: one lane per logical core, containing the same slices placed on the core they *started*
  on (`args.core`). Use it to see work stacking onto one core or threads migrating. Slices under 30 µs are omitted
  from the core view.
- **Synthetic lanes**: `Frames` (one slice per frame), `Main loop phases` and `Main loop sub-phases` (the
  `studio.cpu_timings` phases), `Compose stages` (per-view compose stages), and two GPU lanes: `GPU passes` (GPU time
  of each render-graph pass) and `GPU frames` (first to last GPU timestamp of each frame). See *GPU timing* below.
- **`STALLED in …` instants** on the main thread: one per sampled stack during a stall. `args.stack` holds the
  symbolised call stack. Symbols need the PDB next to the exe — optimised builds emit one (`ORBIT_PROFILER_SYMBOLS`,
  default ON: `/Z7` objects and `/DEBUG` link).

### Settings

`profiler.configure` (or `orbit_profiler_configure`):

| Field | Default | Meaning |
| --- | --- | --- |
| `enabled` | true | Turns scope recording off entirely (scopes become one relaxed load). |
| `hitch_threshold_ms` | 100 | A frame longer than this writes a hitch capture. |
| `stall_threshold_ms` | 250 | Once the current frame exceeds this, the main thread's stack is sampled every ~50 ms. |
| `capture_window_ms` | 4000 | How much history a capture keeps. |
| `max_hitch_files` | 24 | Retained hitch files. |

## GPU timing

`GPU passes` answers "what was the GPU doing while the main thread sat in `vk.fence_wait`?".
`RenderGraph::Execute(commands, &gpuPassTimer)` writes a GPU timestamp after each pass's recording callback
(`engine/render_graph/src/GpuPassTimer.cpp`). They are bottom-of-pipe timestamps, so the gap between two neighbours is
the time the GPU took to finish the pass between them, including time stuck behind earlier work. Results are read
after the frame slot's fence completes (two to three frames later), so GPU spans appear on the timeline slightly after
the CPU frame that recorded them.

- **Placement on the timeline:** the device's `VK_KHR_calibrated_timestamps` pairs the GPU clock with the CPU
  performance counter, so a GPU span lines up exactly with the CPU lanes. Without the extension the spans keep their
  durations but are anchored to when the frame was resolved (`GpuPassTimer::Calibrated()`).
- **Only passes of 50 us or more** are put on the timeline; `GpuPassTimer::LastFrame()` keeps every pass.
- **Panel and RPC:** *Heaviest GPU passes in view*, and `top_gpu_passes` in `profiler.snapshot`.
- **Cost:** one timestamp and one name per pass per frame, about 60 per frame. Turning the profiler off
  (`profiler.configure enabled=false`) stops recording them.
- A pass is a render-graph pass: GPU work submitted outside the graph (the lighting and surface-volume recorders keep
  their own timings) is folded into whichever pass it runs next to.

## Design

- Each thread that records a scope gets a 16384-slot ring of 32-byte events. Recording is a timestamp
  (`QueryPerformanceCounter`), the current core (`GetCurrentProcessorNumber`) and a store: no locks, no allocation.
  Old events are overwritten, so memory is fixed.
- Names must be string literals or `profiler::Intern(...)` pointers.
- `ORBIT_PROFILE_SCOPE("name")` is the only thing instrumenting code needs. `JobSystem` already wraps every job;
  shader compilation (`dxc.compile <entry>`), pipeline/buffer/texture creation, queue submit, fence wait, swapchain
  acquire/present, terrain streaming, globe patch build/upload, physical page products and atmosphere LUT builds are
  instrumented. Add scopes around any new CPU-heavy work.
- Stall stacks use a private dbghelp session with explicit module loads, so another component that already called
  `SymInitialize` for the process cannot silently disable symbolisation.
- The watchdog thread (`Orbit.Profiler`) is started on the main thread (`profiler::StartWatchdog()` in
  `apps/editor`). It suspends the main thread for a few microseconds to read its context, unwinds with
  `RtlVirtualUnwind` and symbolises with dbghelp on the watchdog thread only.
- `profiler::BeginFrame()` / `EndFrame()` bracket each Studio loop iteration (after the intentional idle-pacing
  wait). `CancelFrame()` is used when rendering is skipped (minimised window).

## Hot iteration

The profiler lives in `OrbitCore`. Editing `Profiler.cpp` or adding scopes to other files takes the ordinary
automatic native/Studio-generation handoff described in `ORBIT_HOT_ITERATION.md`; nothing needs a manual restart.
`profiler.configure` changes thresholds live. A failed hot build leaves the running generation (and its ring buffers
and watchdog) untouched.
Changing `ORBIT_PROFILER_SYMBOLS` re-configures CMake and rebuilds every object once.

## RPC / MCP

| MCP tool | RPC method |
| --- | --- |
| `orbit_profiler_status` | `profiler.status` |
| `orbit_profiler_configure` | `profiler.configure` |
| `orbit_profiler_capture` | `profiler.capture` |
| `orbit_profiler_panel_get` | `profiler.panel_get` |
| `orbit_profiler_panel_set` | `profiler.panel_set` |
| `orbit_profiler_snapshot` | `profiler.snapshot` |
