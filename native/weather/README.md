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

The default column contribution weights are `0.77, 0.90, 1.00, 1.07, 1.07, 1.19`. They approximate each level's pressure thickness and are normalized to a mean of one; all six can be changed live from the weather map.

## Coupled surface energy model

The atmospheric lower boundary now carries persistent land/ocean thermal and hydrological state instead of relaxing only toward a latitude curve. `SurfaceEnergy` uploads procedural elevation, water fraction, soil moisture and snow-free albedo after the planet bake, then supplies the live body-fixed Helion direction and N-body irradiance.

Each global and local surface cell evolves:

- surface and subsurface/mixed-layer temperature
- land soil moisture
- snow water equivalent, optical age and wetness
- dynamic albedo
- absorbed shortwave, sensible, latent and ground/deep-water heat fluxes

The energy budget includes slope/aspect solar incidence, cloud attenuation from the six condensate layers, long-wave exchange, two-reservoir thermal inertia, wind-dependent bulk sensible heat exchange, wind-dependent evaporation/dew, precipitation recharge, snowfall, latent heat of fusion and snow insulation. Fresh snow starts near `0.89` albedo, ages toward roughly `0.72`, and wet snow can fall toward roughly `0.60`. Open water uses a much larger mixed-layer heat capacity than land, so the 11.5-hour day produces strong land/ocean thermal contrast without forcing unrealistic daily ocean swings.

Sensible heat is returned to layer 0 potential temperature and evaporation/dew is returned to layer 0 specific humidity, closing the first runtime surface-atmosphere feedback loop. The surface debug RGBA contract is **R** temperature (220–330 K), **G** snow cover, **B** albedo, **A** soil wetness/free-water fraction.

This first pass uses the global procedural macro elevation to derive surface normals and samples that state into the local nest. Fine local terrain horizon occlusion, kilometre/sub-kilometre surface remapping, sea ice and ocean horizontal transport remain separate follow-up systems.

## Runtime physical tuning

Press `é` to open the weather map and `P` to open live physics calibration. Six bounded multipliers alter subsequent native solver steps without reinitializing its state:

The weather map also exposes **Weather speed** from paused `0×` through normal `1×` to rapid spin-up at `128×`. Speed changes the number of calibrated fixed solver steps performed per real second, rather than enlarging the timestep, so fast-forwarding preserves the model's numerical behavior. Values from `16×` upward are labelled spin-up mode. Per-frame catch-up is capped to prevent a frame hitch from causing an unbounded simulation spiral.

- **Circulation** — large-scale wind restoration and pressure-pattern response.
- **Radiative heat** — temperature restoration toward the zonal radiative/surface equilibrium.
- **Humidity supply** — moisture restoration toward a saturation-limited relative-humidity climatology.
- **Cloud physics** — condensation, evaporation, freezing, melting, and condensate decay.
- **Convection** — instability/convergence-driven vertical mass transport.
- **Precipitation** — liquid and ice autoconversion/fallout.

`0×` disables the parameterized process, `1×` is the calibrated baseline, and `2×` doubles its rate or response subject to the solver's safety caps. The separate six altitude controls change cloud-column integration immediately. **Reset Earth-like defaults** restores both groups.

The baseline uses Asterra's locked 11.5-hour rotation rate and 1.10-bar surface pressure, Bolton/Tetens saturation humidity, pressure-dependent potential temperature, latent heat of vaporization and fusion, dry subtropical humidity belts, pressure-thickness layer integration, time-step-independent relaxation, and area-weighted zero-mean global pressure perturbations. These improve plausibility, but this remains a six-level hydrostatic gameplay model rather than a forecast-grade numerical weather prediction system.

### Global

- 256x128x6 atmospheric cells.
- Semi-Lagrangian horizontal transport.
- True latitude-dependent zonal spacing, across-pole ghost topology, and a
  reduced-resolution polar filter for wavelengths the shrinking rings cannot
  resolve.
- Sphere-metric divergence, vorticity, and momentum curvature terms.
- Pressure-gradient acceleration and Coriolis coupling.
- Layer-dependent friction/radiative relaxation and climatological circulation forcing.
- Saturation from temperature/pressure, condensation, evaporation, latent heating, freezing/melting and condensate fallout.

### Local

- 192x192x6 at 2.2 km horizontal spacing (~422 km across).
- Same prognostic variables and physics as global, but shorter timestep.
- Global geographic winds are projected into the fixed local tangent frame,
  including for nests that straddle a pole.
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
