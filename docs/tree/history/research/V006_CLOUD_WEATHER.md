# Orbit V0.0.6 — Cloud Capability and Orbital Weather Representation

Status: M23 implementation baseline.

## Purpose

M23 introduces one cloud/weather authority that is shared by:

- climate-driven cloud placement;
- procedural cloud placement;
- authored/imported coverage adapters;
- orbital cloud appearance;
- surface direct-light shadowing;
- later close-range volumetric/froxel rendering.

Clouds remain a separate capability from the M21 Atmosphere authority.

## Research basis

The architecture follows the established real-time volumetric-cloud pattern of separating:

- a low-frequency weather/coverage field;
- high-frequency volumetric/detail structure;
- lighting/shadow evaluation.

The Nubis/Horizon cloud work is the principal reference for this separation because its weather-map representation carries coverage/type/precipitation authority independently from the expensive volume representation.

M23 implements Orbit's persistent low-frequency authority and derived orbital/shadow products. It intentionally does not make a dense voxel volume the persistent weather source.

## Semantic cloud layers

Cloud Layer remains an ordinary repeatable celestial capability.

Each enabled layer exposes:

- source model;
- base altitude;
- top altitude;
- coverage bias;
- peak optical depth;
- single-scattering albedo;
- phase anisotropy;
- density exponent;
- weather scale;
- detail scale;
- deterministic seed;
- angular wind velocity;
- precipitation phase metadata;
- cloud-shadow participation;
- orbital visibility;
- optional Source Object.

Multiple cloud layers may coexist on one body.

Stable semantic ObjectId halves are carried into derived layer fingerprints.

## Source authority

CloudSourceModel supports:

- Climate Procedural;
- Procedural;
- Authored;
- Imported.

### Climate Procedural

Uses the existing TerrainSource climate authority.

TerrainSample already provides:

- temperature;
- humidity;
- precipitation;
- continentality.

The current M23 coverage baseline uses humidity and precipitation directly, so existing rain shadows and climate revisioning automatically affect cloud fields.

No second climate database is created.

### Procedural

Uses deterministic body-space weather/detail functions driven by:

- seed;
- weather scale;
- detail scale;
- coverage bias.

This path requires no terrain source.

### Authored / Imported

M23 defines CloudCoverageSource:

```
SampleCoverage(unitDirection, SimulationTime)
Revision()
```

An authored or imported Source Object is therefore expected to expose an adapter implementing this contract.

The external source revision participates in the cloud-field fingerprint.

Studio deliberately does not silently substitute procedural weather when an authored/imported adapter is missing.

## Shared cube-sphere weather field

CloudFieldProduct is a derived six-face body-space field.

Each layer stores per texel:

- coverage;
- optical depth;
- single-scattering albedo;
- anisotropy.

The product stores:

- face resolution;
- sample footprint;
- climate revision;
- simulation-time bucket;
- total fingerprint;
- stable layer products.

The same field is sampled by both orbital appearance and cloud-shadow evaluation.

## Temporal evolution

Layer wind is expressed as body-frame angular velocity [rad/s].

Sampling advects the weather coordinate by the inverse angular rotation at the requested simulation time.

The field fingerprint includes a configurable simulation-time quantum.

This prevents frame-rate-dependent weather identity while still allowing deterministic weather animation.

## Fingerprints

Cloud identity includes:

- reference radius;
- field resolution;
- footprint scale;
- time bucket;
- climate revision when used;
- authored/imported coverage revision when used;
- every layer semantic ID;
- source mode;
- geometric layer heights;
- optical/scattering controls;
- noise controls;
- wind;
- shadow/orbital participation.

Derived GPU products are disposable and are never persisted as weather authority.

## Orbital weather representation

GpuCloudFieldProduct packs the complete cube-sphere weather field into a structured GPU buffer.

Studio caches one CPU + GPU cloud product per viewport/body and rebuilds only when its cloud fingerprint changes.

Diagnostics expose:

- body;
- cloud fingerprint;
- climate revision;
- time bucket;
- layer count;
- mean coverage;
- mean optical depth;
- GPU residency.

## Orbital visual integration

For orbital globe/cached-disc rendering, M23 composites orbital-visible cloud layers into the M16 PlanetaryAppearanceProduct.

The composition:

- derives opacity from cloud coverage and optical depth;
- uses layer single-scattering albedo for bright cloud reflectance;
- raises apparent roughness for cloud-covered texels;
- combines the cloud fingerprint into the appearance fingerprint.

The macro-globe cache tracks independently:

- base terrain appearance fingerprint;
- cloud fingerprint;
- final composed appearance fingerprint.

Therefore terrain/climate/cloud changes invalidate only the appropriate derived products.

Layers with Visible From Orbit disabled are skipped by orbital appearance composition.

## Ground/orbit consistency

Ground rendering does not own another cloud map.

The production terrain/weather path and orbital path sample the same CloudFieldProduct authority.

Future close-range volume/froxel rendering should refine the field spatially rather than regenerate different large-scale coverage.

## Cloud shadows

CloudShadowTransmittanceAtSurface evaluates each shadow-participating layer along the body-fixed light direction.

The surface point is projected to the layer's mean spherical shell.

Sampled optical depth is corrected by incidence angle and converted to transmission with Beer-Lambert attenuation:

```
T_cloud = exp(-tau / mu)
```

Multiple participating layers multiply transmission.

## M20 lighting integration

CelestialLightingService now provides DirectLightingAtSurface.

The service first evaluates the ordinary M20 direct source:

- emitter luminosity;
- distance;
- finite-disc celestial eclipses.

It then evaluates M23 cloud transmission at the requested receiver surface direction.

The returned irradiance is:

```
E_surface =
    E_M20 *
    T_cloud
```

This preserves M20 celestial visibility independently from cloud transmission.

Terrain, ocean, vegetation and later atmospheric/froxel consumers can therefore use one direct-light result.

## Recipes

Atmosphere-bearing rocky-planet recipes now also create an ordinary Climate Procedural Cloud Layer.

Seed, coverage bias and optical depth are deterministic recipe outputs.

The resulting cloud capability remains fully editable and uses the normal semantic/Inspector path.

## Validation

Regression coverage includes:

- deterministic climate-derived cloud field generation;
- climate revision participation;
- simulation-time fingerprint changes;
- wind-driven evolution;
- cloud-shadow participation disable;
- semantic multi-layer resolution;
- procedural layer generation without terrain climate;
- orbital appearance fingerprint/composition;
- surface irradiance reduction through CelestialLightingService;
- preservation of M20 celestial visible fraction while clouds attenuate irradiance.

## Intentional limits

M23 does not yet implement the final close-range dense volumetric renderer.

In particular:

- no persistent voxel weather authority;
- no cloud microphysics simulation;
- no convective cell solver;
- no lightning volume lighting;
- no precipitation particle system;
- no cloud self-shadow volume cache;
- no volumetric temporal reconstruction.

Those systems should consume/refine the M23 field rather than create competing large-scale cloud coverage.

The GPU field and orbital composition seam are intentionally compatible with later Nubis-style 3D/froxel/voxel detail systems.

## Simplified planetary weather model

`celestial_clouds/WeatherModel` replaces the uniform sine-noise placement for the Climate Procedural and Procedural
sources (Authored / Imported keep their adapter). `EvaluateWeather(direction, seconds, parameters, climate)` is an analytic,
deterministic function returning coverage, cloud type (0 stratus, 0.5 cumulus, 1 cumulonimbus) and precipitation. It is a
weather *map* authority, not a fluid solver, so it is cheap (about 2.4 microseconds per evaluation) and hot-reloadable.

Structure, in the body-fixed frame (+Y is the pole, latitude = asin(y)):

- **Three-cell circulation.** A convergence zone follows the sub-stellar latitude (`CloudFieldConfig.subsolarLatitudeRadians`,
  fed from the sun direction by Studio, so seasons move it) with organised deep convection clusters that reach
  cumulonimbus; dry subtropical subsidence suppresses cloud and leaves shallow trade cumulus; polar low stratus.
- **Baroclinic storm tracks** in each hemisphere. Two wave trains (about wavenumber 5 and 7) are blended by noise and each
  cyclone gets its own vigor, giving a meandering frontal band with a comma-shaped head and a stratiform shield in each
  trough, so systems differ in size and spacing.
- **Layered cloud** (stratocumulus decks and stratiform sheets) across the mid-latitudes.
- **Climate modulation.** Humidity scales coverage, precipitation adds rain-bearing cloud, temperature pushes wet warm cloud
  towards convective types, so oceans, deserts and rain shadows from the terrain climate show up in the sky.
- Domain-warped multi-octave value noise breaks the bands into natural shapes and slowly evolves with time.

Outputs ride in the existing field: `coverage` and `opticalDepth` as before, `cloudType` and `precipitation` per texel
(the GPU texel carries them in the slots that were constant per layer). The ray-march picks the vertical profile from the
type and thickens and greys precipitating cloud. The cloud field default resolution is now 129 per face. Far away the
renderer fades the billow noise to the mean density so a pixel covering many noise cells does not alias into speckle.

Developer tool: `OrbitWeatherMapDump [subsolarDegrees] [hours] [out.ppm]` writes an equirectangular map
(R coverage, G type, B precipitation) for inspection; `CloudFieldTests` checks determinism, ranges, and that the
convergence zone is cloudier than the subtropics and reaches deep-convective types.

### Orbital look

Rules that keep the clouds readable from orbit (all in `CloudRenderer.cpp` / the field build):

- **Threshold after interpolation.** The GPU texel stores the smooth pre-threshold `weather` value (`CloudTexel.weather`) in
  the coverage slot; the ray-march interpolates it (smoothstep weights) and then applies the same coverage ramp and
  density exponent as the CPU field (constants `lod.yzw`: threshold, peak optical depth, exponent). Edges are smooth contours
  instead of texel-sized steps. The field is 193 per face; the CPU ramp is +/-0.26 around the threshold.
- **Footprint filtering.** Each density sample knows the metres one pixel (or one march step) covers (`t * pixelAngle`).
  The fine erosion fades when the footprint approaches its smallest cell (voxel x 32/12), and the base billow shape fades
  to mean density later, so far clouds are smooth instead of speckled while near clouds keep their billows.
- **Grain.** Interleaved gradient noise jitter plus a 3x3 tent filter in the composite pass.
- **Thickness shading.** Thick cores are brighter and thin edges darker, and the forward-scatter peak is soft-limited, so
  the weather map stays visible and a limb view does not blow out to white.

### Cloud types and the high layer

The cloud-type axis (`cloudType`, per texel) has six anchors; the ray-march interpolates their parameters (`TypeParams`):

| Type | Axis | Top (fraction of shell) | Shape |
|---|---|---|---|
| Stratus | 0.05 | 0.10 | thin, low, smooth |
| Stratocumulus | 0.20 | 0.17 | low lumpy deck |
| Nimbostratus | 0.32 | 0.50 | deep, layered, low erosion, rain-bearing |
| Cumulus | 0.50 | 0.28 | flat base, domed billows, strong erosion |
| Cumulus congestus | 0.72 | 0.52 | tall tower |
| Cumulonimbus | 1.00 | 0.97 | full-height cauliflower tower with taper and a rounded crown |

The march shell is stretched to 2.2x the layer thickness above its base (about 12.5 km for the default 1.5-6.5 km layer) so
towers and cirrus have room. Towers get vertically stretched noise (tall turrets), a per-column top height and a mid-frequency
dome noise. A separate **high layer** (shell fraction 0.74-0.94) renders cirrus and cumulonimbus anvil outflow from the
per-texel `cirrus` coverage (carried in the GPU texel slot that no longer holds optical depth): stretched, fibre-like noise,
peak optical depth 0.7. The weather model produces cirrus from the jet and warm fronts ahead of each cyclone, thin tropical
cirrus, and anvil outflow drifting downwind (west) of deep convection. The per-texel coverage ramp and optical depth are now
computed in the shader from the stored weather value.

Cost control: cheap rejection above the cloud top before any noise fetch, noise fetches only where the shape is non-zero,
dome noise only for towers, and from about 120-300 m of footprint (long range) the exact 4-step sun march blends to a
column-based estimate. At 60 km altitude the pass adds no measurable frame time with the natural weather.

Known limits: cumulonimbus crowns are still blocky rather than convincingly cauliflower-shaped from some angles, anvils are
only seen as a high sheet (no lateral spread from the tower itself), and cirrus has not been inspected in natural weather.

### Cloud lab

`view.terrain_layers_set {cloud_lab: {...}}` (MCP `orbit_view_terrain_layers_set(cloud_lab=...)`) replaces the weather with ONE
isolated cloud placed ahead of the camera along the ground, so a type's vertical development, shape and self-shadowing can be
judged without hunting for it. Parameters: `type` (0.05 stratus, 0.2 stratocumulus, 0.32 nimbostratus, 0.5 cumulus,
0.72 congestus, 1.0 cumulonimbus), `coverage`, `cirrus` (anvil), `precipitation`, `radius_meters`, `height_scale`
(stretches the shell, and the peak optical depth with it so the density per metre is unchanged), `distance_meters`,
`sun_override` with `sun_elevation_degrees` / `sun_azimuth_degrees` (only the camera march uses the lab sun; the terrain keeps
the scene sun), and `place: true`. `cloud_resolution_scale: 1` renders every pixel, which is what you want for judging
edges; the default 0.5 is half resolution.

What the lab found (and fixed): horizontal ribs on towers were the empty-space leap skipping over cloud edges at a
row-dependent phase (now the march backs up to the last empty sample when a leap lands in cloud); smooth towers got a
billow octave (0.4-1.7 km cells) and smoothstep noise interpolation; the in-cloud step is capped at 75 m + 0.006 x distance;
the sun march reaches 3.5 km plus a tail so a wide tower shades its far side.

**Anvil.** The high layer (0.70-0.90 of the shell) is cirrus over non-tower columns and an anvil over towers
(`towerness` from the type axis blends them): the anvil is thick ice (peak optical depth 3 vs 0.7), has a flat underside and a
lumpy top (billow noise mixed into the fibres), and cumulonimbus tops reach 0.76-0.95 of the shell so tower and anvil are one
cloud. Under an anvil the tower flares out into it instead of narrowing to a stem.

Known gaps: thin cirrus still shows faint banding at distance, the stem below a lab anvil is short, and the anvil is driven by
the `cirrus` channel (the model feeds it from deep convection), not simulated outflow.

### Cumulonimbus life cycle (lab)

The cloud lab models a storm cell with `maturity` (0 towering cumulus, 0.3 growing, 0.6 mature, 0.9 dissipating),
`organisation` (1, 3 or 5 cells; daughter cells are younger and smaller; organised storms get a wider anvil), `density`
and `seed`. Per cell the shader derives core coverage (the tower collapses past maturity 0.8) and anvil coverage (spreads
with age, wider when organised). Age drives: tower height (34 % of full when young), glaciation of the crown (soft, more
translucent top from maturity 0.45), mammatus pouches under the anvil (from 0.62), and the anvil only exists from 0.3.
Towers use 12x the fair-weather extinction (the reference clouds use 0.006-0.012 /m for storm types), skin carved by noise,
anvil optical depth up to 9 x density. Lighting follows the reference cloud phase set: single scattering g 0.95 / 0.8 weights
0.1 / 0.2 (the silver lining) with 7x gain, multiple scattering g 0.2 / -0.4 weights 3.0 / 0.3 with extinction scaled
by 0.45.

### Cumulonimbus pass 2 (lighting, anvil, natural weather)

Reference comparison drove these changes (real storm photographs: bright hard-edged turrets, dark flat rain base, fibrous
sloped anvil edge, strong self-shadowing, silver lining):

- **Multiple scattering as octaves** (energy, extinction and anisotropy x0.5 per octave) evaluated on a diffusion-style
  depth `1.6 * sqrt(sunDepth)`, so the inside of a dense tower keeps a white glow instead of going black (a black interior is
  tinted blue by the aerial perspective and the whole cloud reads as a ghost). Single scattering (g 0.95 / 0.8) keeps the
  silver lining; sky light adds a tower-top gradient; a rain-bearing base is darkened; billow crevices (surface relief) are
  darker.
- **Anvil rim**: the underside rises towards the rim (sloped wedge instead of a wall) and the anvil uses the coarse noise (no
  billow octave) with fibres wandering sideways. The concentric ribs on the anvil rim were two march bugs, both fixed:
  the jitter spanned the coarse empty-space step instead of the in-cloud step, and the sun march sampled one fixed set of
  points (now jittered per pixel and filtered).
- **Cirrus**: fibres use the finer baked octaves, tight in latitude and long in longitude, so they read as streaks or
  rippled altocumulus/cirrocumulus instead of kilometre-wide bars.
- **Natural weather**: an anvil exists only where a tower exists (`gAnvilGate`), natural storm extinction is scaled to 0.4 of
  a lab storm, and the cell age is inferred from the anvil coverage.
- Cost: unchanged within noise (about 8.5 vs 8.7 ms at 8 km, 6.3 vs 6.7 ms at 60 km, Release, half resolution).

Capture: `docs/research/cloud_cumulonimbus_terminator.png` (lab cumulonimbus, sun 2 deg above the horizon behind the
cloud, organisation 0.3, cloud_resolution_scale 1).

### Pass 3 (rain, overshoot, anvil tip, cost)

- **Anvil tip streaks**: the thin sheet at the rim was hit by 1-2 samples and aliased into one-pixel rows. The step across the
  high layer now scales with the layer's *thinnest* part, and the fibre noise is filtered by its narrow dimension
  (`fibreBlend`), fading to the noise's true mean density. The mean is `0.53 * coverage^1.6` for cirrus (fitted to the
  threshold ramp); fading to the raw coverage (2-3x too dense) turned the whole planet into a pink haze.
- **Rain shafts**: below the cloud base the march now continues to the ground and `RainExtinction` draws vertical grey
  streaks under heavy precipitating cores only (`rainGate` = precipitation and coverage). Natural weather gets them only
  under strong cores.
- **Overshooting top**: the strongest updraft core rises 5 % above the tower while growing or mature; towers keep a sharper
  crown (the profile fall-off starts at 0.8 of the tower for cumulonimbus) and turret heights vary by about +/-25 %.
- **Sun march**: six steps (120 m to 3 km) plus tail; beyond about 45-140 m of footprint it blends to a column estimate.
  This was the dominant cost (about 37 ms at 60 km before the blend, 5.4 ms after, clouds off 5.3 ms).
- **Natural weather**: anvil optical depth is x0.06 of a lab storm (`gHighDensity`) so continent-sized anvils stay
  translucent; cirrus fibres wander in longitude/latitude without a latitude-proportional shear (that shear made concentric
  moire rings).

### Pass 4 (planet-scale cirrus, wall cloud, storm ages)

- **Cirrus veil**: natural cirrus coverage is remapped to `0.42 * smoothstep(0.12, 0.8, cirrus)` and a thin natural layer peaks
  at optical depth 0.2 (anvil `gHighDensity` 0.03). The model's tropical cirrus is nearly 1 over the whole ITCZ; a slant ray from
  orbit through a planet-wide 2 km layer is hundreds of km long, so even a thin sheet there looked like a pink wall. A sky-wide
  layer must also be seen at its real mean density: the filtered fibre noise now fades to `0.53 * coverage^1.6`.
- **Cirrus shape**: fibres are 24 voxels (about 8 km) long in longitude with a wide, slow wander, so they are layered streaks
  rather than 5 km rectangles. Thin natural anvils use the fibre noise (`anvilCore` scaled by anvil density).
- **Weather lookup warp**: three sine octaves (up to about 50 km) displace the cube-field lookup so bilinear texel edges do not
  show.
- **Wall cloud**: strong mature towers (maturity 0.4-0.78) get a ragged lowered base 1.1 km deep in patches (`WallCloud`).
- **Natural storm ages**: age varies by region (a few hundred km) and by anvil coverage, so a continent carries growing, mature
  and decaying storms.
- Rain is streakier (`0.0016/m`, narrower threshold). Cost: unchanged within noise.
- Build note: the shader source is split across `kCloudCommon..F` literals; a single raw string over about 16 KB fails with
  C2026 (the hot build then silently keeps the previous generation, which cost me an hour of confusing comparisons: always
  check the build log when a change shows no effect).

Known remaining gaps: the organisation control exists only in the lab, the wall cloud and rain shaft are subtle, a thin
sky-wide cirrus layer is deliberately sparse, and rain is a pale haze rather than dark curtains.

## Aliasing, accumulation and the KSP reference (2026-10-04)

Compared against Kerbal Space Program 1.12.5 with Environmental Visual Enhancements (raymarched volumetric clouds,
screenshots in its `Screenshots/`; side-by-side in `cloud_reference_comparison.png`). EVE ships spatiotemporal blue noise
(`stbn.R8`), a cached light volume instead of a per-sample sun march, a 200 m maximum step and signed-distance-field
skipping, and accumulates over frames. Orbit marched at half resolution with one independent random sample per pixel and
no history, so every sampling error was permanent.

- **Temporal resolve** (`CloudRenderer::Resolve`, `view.terrain_layers_set cloud_temporal`, default on): the march jitter
  changes every frame (golden-ratio step), the previous result is reprojected through the cloud shell (rotation and
  translation), clamped to the current 3x3 neighbourhood and blended 20 % new. History resets on resize, lab edits and
  when clouds are off. Cost is below the top-three GPU passes.
- **Stratified jitter**: 4x4 ordered dither with a per-block rotation (no visible period), plus a 3x3 tent in the composite.
- **Noise fades** start at 0.2 cell per pixel (was 0.5) so octaves are gone before the sampling limit.
- **Jitter span** is the step a sample really represents (the in-cloud step), and the sun march is jittered over 0.1-0.9 of a
  step: both removed horizontal terraces on cloud flanks that the converged image exposed.
- Not aliasing, still different from the reference: Orbit has uniform ~5 km Worley lumps and sparse small cells from orbit
  where EVE has wispy, sheared systems; ground-level skies are softer than EVE (half-resolution march).
