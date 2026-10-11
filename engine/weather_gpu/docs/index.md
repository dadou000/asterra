+++
path = "/world/weather-lab/gpu-port"
title = "Weather GPU: the fast storm core as Vulkan compute"
kind = "subsystem"
status = "experimental"
summary = """
GpuStormSolver runs one level of the fast storm core (advection, mass fixer, Kessler + ice microphysics, buoyancy, mixing, sponges, exact FFT pressure projection) as 14 compute kernels in one HLSL file, \
validated against the CPU reference and about 180x faster than it on the 512 km domain. Phase 4 of the storm domain plan: single level, fixed step; it also exports its clouds to the renderer (phase 5) and saves and restores its state bit for bit (phase 6). Nesting kernels are next."""
owner_module = "OrbitWeatherGpu"
keywords = ["weather gpu", "compute", "vulkan", "fast core", "hlsl", "fft", "pressure projection", "gpu port", "hot reload", "storm"]
sources = [
  "engine/weather_gpu/include/orbit/weather_gpu/GpuStormSolver.hpp",
  "engine/weather_gpu/src/GpuStormSolver.cpp",
  "engine/weather_gpu/shaders/storm.hlsl",
  "engine/weather_gpu/tools/WeatherGpuCli.cpp",
  "engine/weather_gpu/tests/WeatherGpuTests.cpp",
  "engine/weather_gpu/CMakeLists.txt",
]
symbols = ["GpuStormSolver", "EmbeddedStormShader"]
applies_to = ["engine/weather_gpu/**"]
invariants = [
  "Each kernel is one stage of weather_lab::FastStormSolver::Impl::Step with the same arithmetic and order; a change to a CPU stage is not done until the matching kernel changes and `orbit_weather_gpu compare` agrees again.",
  "All state is one storage buffer in two ping-pong copies (u, v, w, then scalars theta qv qc qr qi qs qg); a stage reads the current copy and writes in place or into the other copy, and a UAV barrier follows every dispatch. Do not add a second state buffer without moving the barriers.",
  "The pressure solve is float32: the GPU's divergence after projection is 1e-8 to 1.4e-8 /s against the CPU's 4e-9 to 8e-9 in the same storm, which is the accepted tolerance. Do not move it to a looser solver.",
  "The RHI creates every compute pipeline with the entry point `main`; kernels are written as `ENTRY_<name>` and the host defines the chosen one as `main` and the rest as unused names. Adding a kernel means adding it to kKernels in GpuStormSolver.cpp and to the ENTRY fallback list in storm.hlsl.",
  "ReloadShaders swaps the pipelines only when every kernel compiles; a failed edit leaves the running generation usable (tested). Pipelines are only replaced between submissions because Advance waits for its fence.",
  "Save and Restore round-trip the whole state: a restored solver continues bit for bit like the original (tested), which the storm timewarp depends on; keep the kernels free of atomics and unordered reductions.",
  "Grids are limited to 256 cells per axis (one FFT line per workgroup in shared memory) and 120 levels (fixed local arrays); the step is fixed (a Courant limit needs a readback per step) and the storm-following frame is CPU only.",
]
related = ["/world/weather-lab", "/world/weather-lab/global-system-plan", "/world/weather-lab/global-system-status"]
depends_on = ["/world/weather-lab"]
verify = [
  "ctest -R Orbit.WeatherGpu (skips with a message when there is no Vulkan device)",
  "orbit_weather_gpu compare --nx 64 --ny 64 --steps 240 --dt 12 --ice 1 --sponge 6 --bubble 2",
  "orbit_weather_gpu bench --nx 256 --ny 256 --steps 40 --dt 24 --ice 1 --sponge 16",
]
verified = "7eeb1bd"
+++

## What it is

`GpuStormSolver(device, compiler, config)` builds the state buffer, the base-state table (densities, winds, tridiagonal coefficients and FFT eigenvalues, uploaded once) and 14 pipelines from `shaders/storm.hlsl`. `Upload(cpuSolver)` copies a CPU solver's initial state in (the CPU builds the warm bubble), `Advance(steps, dt)` records the steps into one command list and waits, `Download(cpuSolver)` copies the state back. A step is 28 dispatches (with ice): three velocity advections and one scalar advection, the mass fixer (partial sums, ratio, scale), microphysics (one thread per column), buoyancy, top sponge, ten mixing dispatches, the lateral sponge, and the projection (right-hand side, four FFT passes, the tridiagonal solve, the gradient).

## Hot-iteration path (the completion check)

- **Saving `storm.hlsl` while Orbit runs:** the change classifier treats an engine `.hlsl` as an incremental Studio generation refresh, which works but restarts the process generation. The faster path is `ReloadShaders()`: with `ORBIT_WEATHER_GPU_SHADER` set to the file path it re-reads that file, compiles all 14 kernels and swaps; wiring the central watcher to call it is part of the Studio integration, not done here.
- **Saving `GpuStormSolver.cpp`:** a native change; today it needs the generation handoff. The module is a static library with host-owned state (the buffers live in the object), so a reloadable module boundary can be cut at `Advance`.
- **A failed patch:** a shader that does not compile returns the error from `ReloadShaders` and the old pipelines keep running (checked in `WeatherGpuTests.cpp`).
- **State across a reload:** the state buffer is not touched by a pipeline swap, so a shader edit preserves the storm.
- **Old generation and the GPU:** `Advance` waits for its fence, so no submitted work references a replaced pipeline; asynchronous stepping will need deferred retirement.
- **Parity:** no RPC or MCP tool yet; the solver is reached only through the CLI and tests until it is wired into the Weather Lab session, where the `weather_lab.*` methods will select the GPU or CPU solver.

## Diagnose

- *The GPU and CPU runs diverge early:* run `orbit_weather_gpu compare --steps 6`. After one step the fields must agree to about 1e-5; if not, a kernel differs from its CPU stage. Bisect by disabling stages in `RecordStep`. Later growth (0.1 to 0.8 m/s in w after 240 steps) is the storm's chaos amplifying float32 rounding and is expected.
- *Pipeline creation fails with "entry point main not found":* a kernel lost its `ENTRY_` prefix or is missing from the fallback list.
- *The first dispatch hangs or the device is lost:* run with `enableValidation` in `CreateDevice`; out-of-range indices in a storage buffer show up as validation errors, not crashes.
- *Divergence after projection grows:* a stage after `ProjGradient` modifies u, v or w, or the lateral sponge ran after the projection instead of before it.
