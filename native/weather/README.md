# Asterra AVX2 Weather Core

This extension is the primary meteorology backend for `weather/0.0.5`.

## Build on Windows

The simplest route is the repository helper:

```powershell
powershell -ExecutionPolicy Bypass -File tools/build_weather_avx2_windows.ps1
```

With a `godot-cpp` checkout matching the custom Godot 4.7.1 fork:

```powershell
powershell -ExecutionPolicy Bypass -File tools/build_weather_avx2_windows.ps1 -GodotCppDir C:/src/godot-cpp
```

You can also invoke CMake directly:

```powershell
cmake -S native/weather -B native/weather/build
cmake --build native/weather/build --config Release --target asterra_weather --parallel
```

The build writes both runtime files directly into `bin/`:

- `bin/asterra_weather.dll`
- `bin/asterra_weather.gdextension`

The manifest is copied only after a successful native build. `WeatherNativeBootstrap` loads it before `WeatherSystem`; an unbuilt checkout therefore boots with neutral fallback weather instead of failing on a missing native class.

## CPU requirement

The extension is intentionally AVX2 + FMA only (`/arch:AVX2` on MSVC, `-mavx2 -mfma` on GCC/Clang). There is no scalar solver backend on this branch.

## Six-layer atmosphere

Global and local grids use the exact same six terrain-following sigma levels:

| Layer | Sigma | Approx. standard-atmosphere height |
| ---: | ---: | ---: |
| 0 | 0.95 | 0.45 km |
| 1 | 0.82 | 1.7 km |
| 2 | 0.68 | 3.3 km |
| 3 | 0.52 | 5.6 km |
| 4 | 0.36 | 8.5 km |
| 5 | 0.20 | 12.8 km |

The prognostic state is layer-major SoA. Each layer stores potential temperature, specific humidity, tangent-space east/north wind (`u`, `v`), liquid condensate, ice condensate and pressure/geopotential anomaly. No yaw/pitch or XYZ wind is stored.

Five interface fields carry vertical mass flux. Horizontal convergence, static instability and moisture drive ascent; precipitation loading can drive downdrafts. Vertical exchange transports heat, moisture, momentum and condensate between the same matching layers in both grids.

### Global

- 256x128x6 atmospheric cells.
- Semi-Lagrangian horizontal transport.
- Pressure-gradient acceleration and Coriolis coupling.
- Layer-dependent friction/radiative relaxation and climatological circulation forcing.
- Saturation from temperature/pressure, condensation, evaporation, latent heating, freezing/melting and condensate fallout.

### Local

- 192x192x6 at 2.2 km horizontal spacing (~422 km across).
- Same prognostic variables and physics as global, but shorter timestep.
- Outer 24-cell rim is smoothly nudged toward the matching global sigma layers.
- The nest remains spatially fixed until the player moves ~18% of its width, avoiding the previous frame-by-frame rotation of local data.

## Derived dynamics

Vorticity is not duplicated as primary state. Every dynamics step derives:

- horizontal divergence
- relative vertical vorticity `dv/dx - du/dy`
- absolute vorticity through the latitude-dependent Coriolis parameter
- a simplified potential-vorticity proxy `(relative vorticity + f) * d(theta)/d(sigma)`
- adjacent-layer vertical wind shear

These diagnostics participate in the organised-storm output and are published separately for tuning.

## GPU/UI contracts

The renderer-compatible RGBA32F weather textures remain:

- **R** = vertically integrated cloud/coverage potential
- **G** = organised-storm signal from ascent + instability + rotation + shear
- **B** = current precipitation intensity
- **A** = normalized boundary-layer pressure anomaly

A second RGBA32F diagnostic texture uses:

- **R** = signed relative vorticity, neutral at 0.5
- **G** = signed low-level divergence, neutral at 0.5
- **B** = signed upper-level PV proxy, neutral at 0.5
- **A** = maximum column vertical wind shear

Open the live map with `é`. Products 6–9 display these new dynamics. The simulation-influence slider rescales exported weather/diagnostic anomalies without altering the solver timestep or internal numerical state.
