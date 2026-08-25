# Asterra AVX2 Weather Core

This extension is the primary meteorology backend for `weather/0.0.5`.

## Build on Windows

With CMake 3.20+ you can build directly and let CMake fetch `godot-cpp` master:

```powershell
cmake -S native/weather -B native/weather/build -DCMAKE_BUILD_TYPE=Release
cmake --build native/weather/build --config Release
```

For the custom Godot 4.7.1 fork, prefer an exact matching `godot-cpp` checkout when available:

```powershell
cmake -S native/weather -B native/weather/build -DGODOT_CPP_DIR=C:/src/godot-cpp -DCMAKE_BUILD_TYPE=Release
cmake --build native/weather/build --config Release
```

The build emits `bin/asterra_weather.dll`. The `.gdextension` manifest is already in the project and Godot loads the native `WeatherNative` class at startup.

## CPU requirement

The extension is intentionally compiled with AVX2 + FMA (`/arch:AVX2` on MSVC, `-mavx2 -mfma` on GCC/Clang). There is no scalar runtime backend on this branch.

## Model

- **Global:** 256x128 spherical/equirectangular SoA state. Temperature, pressure anomaly, humidity, horizontal wind, condensed water, CAPE, vorticity and precipitation are evolved with semi-Lagrangian transport, pressure-gradient acceleration, Coriolis coupling, convergence, condensation/latent heating and convective CAPE release.
- **Local:** 192x192 tangent-plane nest at 2.2 km/cell (~422 km across), centered on the player. It is weakly nudged to the global state while retaining stronger mesoscale convergence/convection.
- **Renderer contract:** RGBA32F weather textures use R=cloud/coverage potential, G=storm/convection, B=precipitation, A=normalized pressure. The cloud compute shader uses these fields for placement, vertical development and optical density; 3D noise is retained only for sub-weather-scale cloud morphology.
