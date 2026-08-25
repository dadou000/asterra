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

The build writes both files directly into `bin/`:

- `bin/asterra_weather.dll`
- `bin/asterra_weather.gdextension`

The manifest is intentionally copied only after a successful native build. `WeatherNativeBootstrap` checks for that manifest and loads it before `WeatherSystem`. An unbuilt checkout therefore starts with a neutral weather field instead of failing GDScript parsing or trying to load a missing DLL.

After the first successful build, restart Godot.

## CPU requirement

The extension is intentionally compiled with AVX2 + FMA (`/arch:AVX2` on MSVC, `-mavx2 -mfma` on GCC/Clang). There is no scalar runtime backend on this branch.

## Model

- **Global:** 256x128 spherical/equirectangular SoA state. Temperature, pressure anomaly, humidity, horizontal wind, condensed water, CAPE, vorticity and precipitation are evolved with semi-Lagrangian transport, pressure-gradient acceleration, Coriolis coupling, convergence, condensation/latent heating and convective CAPE release.
- **Local:** 192x192 tangent-plane nest at 2.2 km/cell (~422 km across), centered on the player. It is weakly nudged to the global state while retaining stronger mesoscale convergence/convection.
- **Renderer contract:** RGBA32F weather textures use R=cloud/coverage potential, G=storm/convection, B=precipitation, A=normalized pressure. The cloud compute shader uses these fields for placement, vertical development and optical density; 3D noise is retained only for sub-weather-scale cloud morphology.

## Runtime influence

Open the live weather map with `é`. The simulation-influence slider updates every
weather consumer immediately: 0× produces a neutral mostly-clear field, 1× is
the calibrated model, and 2× amplifies its anomalies. The control changes visual
weather strength, not the model timestep, so its numerical evolution remains
stable and deterministic.
