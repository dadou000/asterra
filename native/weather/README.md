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

The atmospheric lower boundary carries persistent land/ocean thermal and hydrological state instead of relaxing only toward a latitude curve. `SurfaceEnergy` uploads procedural geography and then supplies the live body-fixed Helion direction, instantaneous N-body irradiance and Helion angular radius.

Each global and local surface cell evolves:

- surface and subsurface/mixed-layer temperature
- land soil moisture
- snow water equivalent, optical age and wetness
- dynamic albedo
- absorbed shortwave, sensible, latent and ground/deep-water heat fluxes

The energy budget includes terrain slope/aspect solar incidence, six-layer cloud attenuation, terrain-horizon occlusion in the local nest, sky-view attenuation of diffuse light, long-wave exchange, two-reservoir thermal inertia, mechanical plus free-convective sensible exchange, evaporation/dew, precipitation recharge, snowfall, latent heat of fusion and snow insulation. Fresh snow starts near `0.89` albedo, ages toward roughly `0.72`, and wet snow can fall toward roughly `0.60`. Open water uses a much larger mixed-layer heat capacity than land, so Asterra's 11.5-hour day produces strong land/ocean thermal contrast without unrealistic daily ocean swings.

Sensible heat is returned to layer 0 potential temperature and evaporation/dew is returned to layer 0 specific humidity. Positive surface sensible heat also enters the lowest vertical mass-flux interface explicitly, allowing differential slope heating to create resolved buoyant ascent rather than waiting only for the slower static-instability feedback.

### Local terrain coupling

The 192x192 local weather nest receives its own procedural surface build rather than simply resampling the coarse global surface. The build:

- samples the same runtime terrain-height function used by the world
- includes sparse player terrain deltas through a thread-safe snapshot
- uses real lake/ocean free-water elevation
- derives terrain normals with a 600 m geodesic footprint
- is generated on a worker thread and coalesces repeated recenter/edit requests
- rebuilds when the native local nest recenters or terrain inside it is edited

Native code then constructs a 24-azimuth terrain horizon for every local surface cell. Ten increasing march distances reach approximately 121 km from a cell where the nest permits it. Helion visibility is compared against this horizon using the star's actual angular radius, so sunrise/sunset over mountains is softened by the stellar disc rather than switched with a binary point-light test. The same horizon supplies an approximate diffuse sky-view factor.

The surface debug RGBA contract is **R** temperature (220–330 K), **G** snow cover, **B** dynamic albedo, **A** soil wetness/free-water fraction.

## Dynamic terrain appearance

The active procedural terrain shader samples the global and nested local surface-energy textures. Current snow cover therefore accumulates, ages, wets and melts visually instead of being only a static biome tint. Wet ground darkens and melting snow becomes less rough/more specular. The generated frost field remains only a weak permanent/glacial baseline.

## Unified physical clock

`CelestialSystem.simulation_seconds` is authoritative. Global weather, local weather and the surface model now consume the same elapsed Asterra seconds while keeping their numerically stable fixed timesteps (90 s global, 20 s local). The speed control changes `CelestialSystem.time_scale`; it no longer creates separate global/local effective weather clocks.

Press `é` to open the weather map. **Weather speed** uses logarithmic powers of two from paused `0x` through normal `1x` to `8192x` global spin-up. The helper beside it shows simulated duration per real second and cumulative warped time in seconds, minutes, hours, or days. Above `256x`, the expensive 2.2 km nest pauses while the whole-planet atmosphere advances with the native stable 180-second step; returning to interactive speed rebuilds the nest from the evolved global state. A pathological external time jump may leave a backlog capped at 64 solver steps per rendered frame; elapsed physical time is retained rather than silently discarded.

The map also exposes synoptic H/L analysis, cloud, precipitation, organised storms, pressure, near-surface air temperature, sea-surface temperature, CAPE, absorbed solar irradiance, vorticity, divergence, potential vorticity and wind shear. Synoptic mode identifies depressions, anticyclones, storm lows and warm-core hurricane/typhoon candidates from the live pressure, rotation, rain, CAPE and sea-temperature fields.

## Runtime physical tuning

Press `P` from the weather map to open live physics calibration. Six bounded multipliers alter subsequent native solver steps without reinitializing its state:

- **Circulation** — large-scale wind restoration and pressure-pattern response.
- **Radiative heat** — temperature restoration toward the large-scale radiative equilibrium.
- **Humidity supply** — moisture restoration toward a saturation-limited climatology.
- **Cloud physics** — condensation, evaporation, freezing, melting, and condensate decay.
- **Convection** — instability/convergence/surface-buoyancy-driven vertical transport.
- **Precipitation** — liquid and ice autoconversion/fallout.

`0x` disables the parameterized process, `1x` is the calibrated baseline, and `2x` doubles its rate or response subject to solver safety caps. The separate six altitude controls change cloud-column integration immediately.

The baseline uses Asterra's locked 11.5-hour rotation rate and 1.10-bar surface pressure, Bolton/Tetens saturation humidity, pressure-dependent potential temperature, latent heat of vaporization and fusion, pressure-thickness layer integration, time-step-independent relaxation, and area-weighted zero-mean global pressure perturbations. It remains a six-level hydrostatic gameplay atmosphere rather than a forecast-grade NWP model.

### Global

- 256x128x6 atmospheric cells.
- Semi-Lagrangian horizontal transport.
- True latitude-dependent zonal spacing, across-pole ghost topology, and a reduced-resolution polar filter for unresolved polar wavelengths.
- Sphere-metric divergence, vorticity, and momentum curvature terms.
- Pressure-gradient acceleration and Coriolis coupling.
- Layer-dependent friction/radiative relaxation and climatological circulation forcing.
- Saturation from temperature/pressure, condensation, evaporation, latent heating, freezing/melting and condensate fallout.
- Persistent global land/ocean/snow surface reservoirs.

### Local

- 192x192x6 at 2.2 km horizontal spacing (~422 km across).
- Same atmospheric variables and physics as global, with a shorter fixed timestep.
- Global geographic winds are projected into the fixed local tangent frame, including polar nests.
- Outer 24-cell rim is smoothly nudged toward the matching global sigma layers.
- The nest remains spatially fixed until the player moves about 18% of its width.
- Independent procedural terrain surface, sub-kilometre normals and 24-direction terrain horizons.

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

The severe-weather probe also consumes a convective RGBA32F diagnostic texture:

- **R** = strongest resolved upward mass flux
- **G** = strongest downdraft
- **B** = upper-level ice/anvil reservoir
- **A** = near-surface wind speed

## Severe-weather validation

`tests/WeatherSevereProbe.tscn` performs scale-aware structural tests rather than
checking only finite array values. The 256x128 global mesh is tested for closed,
rotating warm-ocean lows, hurricane-threshold wind, pressure closure, anvils, and
linear mesoscale convection. The 2.2 km nest is tested for deep-convective
lifecycle, cold-pool-organized lines, and persistent rotating updrafts in shear.
For example:

```powershell
godot --headless --path . tests/WeatherSevereProbe.tscn -- --days=15 --local-hours=6 --seed=1095980101 --output=res://tests/weather_severe_output
```

Runs of four or more global days and six or more local hours fail with a nonzero
exit code when a required severe structure is absent or the local nest runs away.
The global grid represents hurricanes through a bounded coarse-grid warm-core /
eyewall closure; it cannot resolve an eye wall explicitly. “Supercell-like” means
a persistent rotating deep updraft within the nest's roughly 2.2 km / six-level
resolution, not tornado-scale dynamics or forecast-grade storm morphology.

Open the live map with `é`. Products 6–9 display these dynamics. The simulation-influence slider rescales exported weather/diagnostic anomalies without altering the solver timestep or internal numerical state.
