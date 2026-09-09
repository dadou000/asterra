# Raymarched volumetric clouds — how the system works

A text schematic of Blackrack's EVE Raymarched Volumetrics (the
`layerRaymarchedVolumeV5` path), reconstructed from the config vocabulary in
this folder plus the standard "cloud modeling & rendering" technique it
implements (Schneider/Guerrilla *Nubis*, plus a precomputed light volume for
multiple scattering). Config-backed facts are marked with the key name;
everything else is the established method the keys parametrise.

Read alongside [`GameData/StockVolumetricClouds/Clouds/clouds.cfg`](GameData/StockVolumetricClouds/Clouds/clouds.cfg)
(the layer/type definitions) and [`README.md`](README.md) (the distilled look).

---

## 0. Top-level data flow

```
                      ETERNAL INPUTS                         PER-FRAME
  ┌───────────────────────────────────────────┐   ┌───────────────────────────┐
  │ coverageMap   (2D, sphere, R = coverage)  │   │ camera pos / view ray     │
  │ cloudTypeMap  (2D, sphere, R = type idx)  │   │ sun direction             │
  │ cloudColorMap (2D, optional tint)         │   │ time  (advects noise)     │
  │ detail1  (3D Worley/Perlin erosion vol)   │   │ blue noise slice (STBN)   │
  │ uvnoise1 (2D domain-warp)                 │   └───────────┬───────────────┘
  │ per-type curves: coverageCurve,           │               │
  │                  densityCurve             │               │
  │ raymarchingSettings, phaseFunctions       │               │
  └───────────────────┬───────────────────────┘               │
                      │                                       │
                      ▼                                       ▼
        ╔═══════════════════════════╗            ╔══════════════════════════╗
        ║ A. LIGHT VOLUME (slow)    ║            ║ B. VIEW RAYMARCH (fast)  ║
        ║  low-res 3D grid, 224 px  ║  sampled   ║  per screen pixel, into  ║
        ║  wide, filled over 12     ║ ─────────► ║  the cloud shell, jitter ║
        ║  frames. Stores in-scat + ║   by B     ║  + adaptive steps.       ║
        ║  multi-scatter energy.    ║            ║  Density from §2, light  ║
        ╚═══════════════════════════╝            ║  from §3.                ║
                                                ╚═══════════┬══════════════╝
                                                            │ RGBA: scattered
                                                            │ colour + transmittance
                                                            ▼
        ╔══════════════════════════╗            ╔══════════════════════════╗
        ║ D. SCALED-SPACE 2D LAYER ║  cross-    ║ C. ATMOSPHERE COMPOSITE  ║
        ║  textured sphere, used    ║  fade     ║  aerial perspective on    ║
        ║  from orbit. scaledFade   ║ ◄───────► ║  the cloud buffer, then   ║
        ║  Start/End Altitude.      ║  200–260km ║  god-rays from transmit.  ║
        ╚══════════════════════════╝            ╚═══════════┬══════════════╝
                                                            ▼
                                             blit over the frame; feed cloud
                                             transmittance to sun-light
                                             extinction, terrain shadow,
                                             particleField / droplets / wetSurfaces
```

Three raymarched layers stack on Kerbin and composite in `overlapRenderOrder`:
`Kerbin-base-layer` (alt 2200), `Kerbin-cumulonimbus-layer` (alt 10000,
`overlapRenderOrder = 2`), and `Kerbin-Weather-1/2` (alt 2200, storm cells).

---

## 1. The cloud domain

Each `OBJECT` is one **layer** = a spherical shell concentric with the planet.
`altitude` is the layer's nominal reference height. The shell's real vertical
extent is the min/max of its **cloud types**: every `cloudTypes > Item` has
`minAltitude` / `maxAltitude` (e.g. Stratus 1750–3450 m, Cumulus 1750–4450 m,
Congestus up to ~8 km, cumulonimbus `trail` 1750–11450 m) and
`interpolateCloudHeights` to blend a type's band with its neighbours instead of
hard-cutting.

Sampling is done in **spherical coordinates** (`noise > worley > spherical = 1`)
so noise cells stay square-ish over the whole globe with no polar pinch or
cube-face seams.

Define, for a sample point `P`:

- `h_norm` = normalised height in the local type band,
  `(altitude(P) - minAltitude) / (maxAltitude - minAltitude)`, clamped 0..1.
- `uv_sphere` = direction(P) → lat/long, used for all 2D map reads.

---

## 2. Density field  `ρ(P)`

The raymarch needs a scalar density at any `P`. It is built in this order:

### 2.1 Coverage and type (2D, cheap, gates everything)

```
cov_raw  = coverageMap.sample(uv_sphere).r            # AlphaMap, ALPHAMAP_R
typ      = cloudTypeMap.sample(uv_sphere).r           # 0..1 → index into cloudTypes
```

`painterTiles` (and `painterTileMasks`) overlay a local, higher-res
coverage/type patch — e.g. `PressureSystemTop`, `size = 600000` m — remapped by
`remapCoverage` / `remapType` and blended onto the globals. This is how a
pressure system or a hand-painted storm is dropped onto the base field.

`typ` selects (and, between integer values, **interpolates**) two `cloudTypes`
items. All following per-type params (`density`, `baseNoiseTiling`,
`noiseEdgeHardness`, curves, …) are the interpolated blend.

### 2.2 Coverage curve (art-directable remap)

```
cov = coverageCurve.evaluate(cov_raw)                 # Unity AnimationCurve
```

`coverageCurve` keys are `time value inTangent outTangent` (Hermite). This lets
each type turn raw coverage into an S-curve, a threshold, or a
"thin-then-sudden" onset (see Stratus vs Cumulus vs cumulonimbus `core` in
`clouds.cfg`).

### 2.3 Base shape noise (3D, low frequency)

```
p        = P + domainWarp(uvnoise1, _UVNoiseStrength)          # break tiling
p       += curlNoise(tiling, strength) * curlNoiseStrength     # swirl / wisps
base_fbm = worleyFBM(p / baseNoiseTiling,
                     octaves, persistence)                     # spherical Worley fBm
```

- `baseNoiseTiling` is the **world-space size in metres** of one base noise cell
  (2300 m for Kerbin cumulus; 50000 m for the cumulonimbus `trail`).
- `persistence = 0.57` — amplitude ratio between fBm octaves.
- `worley` (cellular) noise gives the billowy, rounded cauliflower silhouette;
  inverted Worley = puffs.

The base cloud mass is `cov` eroded by `base_fbm`, then hardened:

```
shape   = remap(base_fbm, 1.0 - cov, 1.0, 0.0, 1.0)           # coverage cuts the noise
shape   = pow(shape, mix(1.0, 8.0, noiseEdgeHardness))        # 0.9–0.97 → crisp rim
```

### 2.4 Detail erosion (3D, high frequency, edges only)

```
det      = fbm(detail1 sampled at p * _DetailScale,           # _DetailScale = 30
               scroll = detailSpeed * time)
detail   = mix(det, 1.0 - det, saturate(h_norm))              # curl direction flips with height
shape   -= detail * erosionDepth * (1.0 - shape)              # eat only the fringes
```

`erosionDepth = 0.65`, `detailNoiseStrength` scale it. Eroding proportionally to
`(1 - shape)` keeps cores solid and only frays the boundary — the classic
"whispy edge, dense middle" look.

### 2.5 Vertical profile and final density

```
prof     = densityCurve.evaluate(h_norm)                      # per-type height shaping
ρ(P)     = saturate(shape) * prof * density                   # 'density' e.g. 0.0015 stratus,
                                                              #                0.012  cumulus
```

`densityCurve` is usually "0 at the base → 1 at the top" for cumuliform
(anvils), flat for stratiform. `density` is the extinction coefficient the
raymarch integrates.

---

## 3. Lighting model

### 3.1 Direct sun — the light march

At each populated raymarch step, take a short secondary march **toward the sun**:

```
τ_sun = 0
for k in 1..lightMarchSteps:                                  # 4 (base), 0 disables
    s   = lightMarchDistance / lightMarchSteps                # 1200 m total
    τ_sun += ρ(P + sunDir * s*k) * s
sun_T   = exp(-τ_sun * density)                               # Beer
powder  = 1 - exp(-2 * τ_sun * density)                       # dark-edge / powder term
direct  = sun_T * mix(1.0, powder, powderWeight)
```

`lightMarchSteps = 0` on the cumulonimbus **top** layer: no per-step sun march,
lighting comes entirely from the light volume (§3.3) — much cheaper for the
towering anvil.

### 3.2 Phase function — the silver lining

Scattering angle `θ` between view ray and sun. Dual-lobe Henyey–Greenstein,
evaluated as a weighted sum; `singleScattering*` and `multipleScattering*` are
`(g, weight)` pairs:

```
phase(θ) =  0.95w·HG(θ, 0.10)  + 0.80w·HG(θ, 0.20)            # singleScattering1/2 (base layer)
          + 0.20w·HG(θ, 3.00)  + (-0.40)w·HG(θ, 0.30)         # multipleScattering1/2
```

Forward `g` near 0.8–0.95 → bright rim when looking toward the sun through a
cloud edge. The negative-`g` term adds a little backscatter glow. Weights are
relative, not normalised (MS weight of 3.0 is intentional). Cumulonimbus uses
tighter lobes (`0.95,0.05` / `0.8,0.15`).

### 3.3 Multiple scattering — the light volume  ★

`lightVolumeSettings` + [`raymarchedClouds.cfg`](GameData/StockVolumetricClouds/raymarchedClouds.cfg):

- A **low-resolution 3D grid** wrapping the cloud shell, `horizontalResolution =
  224` cells across.
- Each cell stores integrated **in-scattered + multiple-scattered radiance**
  (an isotropic energy estimate), computed by its own cheap marches.
- Refilled incrementally: `directLightTimeSlicing = 12` spreads the update over
  12 frames; `maxTimewarpUpdateRateIncrease = 3` lets it refresh faster under
  time warp.
- The view raymarch (§4) then **samples this volume** trilinearly instead of
  marching for MS — O(1) per step. This is what makes deep clouds glow from
  within without a prohibitive nested march.

### 3.4 Ambient / sky

```
sky   = skylightMultiplier * atmosphereAmbient(P)             # 1.0
sky  *= mix(vec3(1), atmosphereTint(P), skylightTintMultiplier)   # 0.7 → tinted by air
amb   = sky * ambientVolume                                   # per-type, e.g. 2
```

`multipleScatteringBrightness` (e.g. 0.8) is a final gain on the volume term;
`color` (120,120,120,255) and `brightness` tint/expose the whole layer;
`_Lambertian = 0.75` only affects the 2D fallback material.

### 3.5 Composite at a step

```
L_step   = (direct * phase(θ) + amb + volumeMS(P) * multipleScatteringBrightness)
           * lightColour
```

---

## 4. The view raymarch

Per screen pixel, once the ray is intersected against the shell:

```
t        = t_near + STBN(pixel, frameSlice) * baseStepSize    # blue-noise start jitter
step     = baseStepSize                                       # 75 m base / 300 m high layers
T        = 1.0                                                # transmittance
C        = 0.0                                                # scattered colour
while t < t_far and T > 0.003:
    P    = rayOrigin + rayDir * t
    d    = ρ(P)                                               # §2
    if d > 0:
        L      = lightingAtStep(P)                            # §3.5
        a      = 1 - exp(-d * step * density)                 # Beer–Lambert slab
        C     += T * a * L
        T     *= 1 - a
    step = min(step * (1 + adaptiveStepSizeFactor * t),       # 0.006  → cone growth
               maxStepSize)                                   # 1500 m
    t   += step
outPixel = vec4(C, T)                                         # colour + transmittance
```

- **Adaptive step**: near clouds are sampled at `baseStepSize`; the step grows
  with distance so the horizon is cheap. `continuousAccumulationDistance`
  controls the range over which alpha ramps so LOD changes don't pop.
- **Under-sampling + reprojection**: only a fraction of pixels are fully marched
  per frame; the rest are reprojected from history and blended. Scatterer's TAA
  (`taaJitterSpread = 0.8`, `taaMotionBlending`, `taaSharpness`) resolves the
  blue-noise grain. `directLightTimeSlicing` does the same for the light volume.
- **Layer overlap**: when base + cumulonimbus + weather shells intersect the
  ray, their marches are merged front-to-back in `overlapRenderOrder`.

---

## 5. Atmosphere integration (Scatterer)

[`GameData/Scatterer/config/config.cfg`](GameData/Scatterer/config/config.cfg):

- `integrateWithEVEClouds = True` — the cloud RGBA buffer is composited **inside**
  the atmosphere pass, so **aerial perspective** (distance in-scatter + air
  extinction) is applied to clouds exactly as to terrain. Far clouds go blue and
  lose contrast.
- `useRaymarchedCloudGodrays = True`, `raymarchedGodraysStepCount = 50` —
  screen-space crepuscular rays marched through the cloud **transmittance**
  buffer.
- `sunlightExtinction = True` — the same cloud transmittance dims the directional
  sun used for terrain/ocean/craft lighting, so flying under an overcast
  actually darkens the ground.
- `disableAmbientLight = True` — stock ambient is replaced by Scatterer's
  physically-based sky term (the one §3.4 samples).

---

## 6. Scaled-space handoff (the LOD that hides the cost)

Two representations of the same clouds:

| Range | Representation | Assets |
| --- | --- | --- |
| surface … `scaledFadeStartAltitude` (200 km) | full raymarch (§4) | noise volumes only |
| 200 km … `scaledFadeEndAltitude` (260 km) | cross-fade of the two | — |
| above 260 km | 2D lit sphere (`layer2D` / `macroCloudMaterial`) | `_MainTex = .../scaled` (albedo), `_BumpMap = .../normals` (relief), `_DetailTex` + `_DetailDist` for near detail |

The `scaled`/`normals` `.dds` are pre-rendered orbital cloudscapes (the 179 MB
files omitted from this copy). `settings._Color` (e.g. 512,512,512,255 —
overbright) and `_BumpScale` tune the orbital look. The raymarcher never runs
from orbit; the sphere never shows up close.

---

## 7. Motion

| Key | Meaning | Example |
| --- | --- | --- |
| `speed` | layer rigid rotation, deg/s per axis (prevailing advection) | `0, 30.39, 0` (cumulonimbus) |
| `detailSpeed` | independent scroll of the detail noise | `0, 6, 0` |
| `upwardsCloudSpeed` | vertical development rate (convection) | `5` |
| `_UVNoiseAnimation`, `_UVNoiseScale` | animate the domain-warp | — |
| `curlNoise.tiling/strength` + `curlNoiseStrength` | static swirl of sample pos | `20000 / 10000`, contrast `0.25` |

---

## 8. Downstream effects (driven by cloud coverage, not part of the render)

- **`shadowMaterial` / `_VolumetricShadowDensity` / `_ShadowFactor = 1`** — clouds
  cast shadows onto terrain, ocean and craft. `shadows.cfg` only lists
  *cross-body* casters (Kerbin shadowing Mun, etc.).
- **`particleField`** ([`particleFields.cfg`](GameData/StockVolumetricClouds/Clouds/particleFields.cfg))
  — GPU particle precip beneath cloud where `coverage > minCoverageThreshold`:
  `rain-Kerbin` = 200k particles, `fallSpeed 12`, sprite sheet + `splashes`;
  `snow-Kerbin` = 400k, `fallSpeed 0.75`; `dust-Duna`.
- **`droplets`** ([`droplets.cfg`](GameData/StockVolumetricClouds/Clouds/droplets.cfg))
  — camera-canopy droplet/streak shader when the camera is inside precip or
  cloud; triplanar, speed-reactive streak ratio, `dryingSpeed`.
- **`wetSurfaces`** ([`wetSurfaces.cfg`](GameData/StockVolumetricClouds/Clouds/wetSurfaces.cfg))
  — per-surface-class (scenery / terrain / craft) wetness + puddle accumulation
  integrator: `wetnessAccumulationSpeed`, `puddleDryingSpeed`,
  `minCoverageThreshold = 0.15`, `puddleTextureScale`.
- **`lightning`** — electrical FX + light flashes for the `Thunder` cloud type
  (`lightningConfig`, `lightningFrequency`).

---

## 9. Per-layer parameter map (`clouds.cfg` → stage)

| Block / key | Stage | Note |
| --- | --- | --- |
| `settings._MainTex/_BumpMap` | §6 scaled 2D | orbital textures |
| `settings._DetailTex/_DetailScale` | §2.4 | 3D erosion noise |
| `settings._UVNoiseTex/_UVNoiseStrength` | §2.3 | domain warp |
| `settings._Color/_BumpScale/_Lambertian` | §6 / §3.4 | 2D layer look |
| `layer2D.shadowMaterial.*` | §8 | cloud→ground shadow |
| `layer2D.macroCloudMaterial._DetailDist` | §6 | 2D detail fade distance |
| `layerRaymarchedVolumeV5.color / brightness` | §3.4 | layer tint/exposure |
| `…​.skylightMultiplier / skylightTintMultiplier` | §3.4 | ambient sky |
| `…​.scaledFadeStart/EndAltitude` | §6 | raymarch↔2D crossfade |
| `raymarchingSettings.baseStepSize / maxStepSize / adaptiveStepSizeFactor` | §4 | primary march |
| `raymarchingSettings.lightMarchSteps / lightMarchDistance` | §3.1 | sun march |
| `raymarchingSettings.overlapRenderOrder` | §4 | multi-layer merge |
| `noise.erosionDepth` | §2.4 | edge erosion amount |
| `noise.worley.spherical / persistence` / `octaves` | §2.3 | base fBm |
| `curlNoise.* / curlNoiseStrength` | §2.3 | swirl |
| `coverageMap / cloudTypeMap / cloudColorMap` | §2.1 | 2D fields |
| `painterTiles / painterTileMasks / remapCoverage / remapType` | §2.1 | local overlays |
| `cloudTypes.Item.density` | §2.5 | extinction coeff |
| `…​.minAltitude / maxAltitude / interpolateCloudHeights` | §1 | vertical band |
| `…​.baseNoiseTiling` | §2.3 | base cell size (m) |
| `…​.noiseEdgeHardness` | §2.3 | rim sharpness |
| `…​.coverageCurve` | §2.2 | coverage remap |
| `…​.densityCurve` | §2.5 | vertical profile |
| `…​.ambientVolume / multipleScatteringBrightness` | §3.4/§3.5 | MS/ambient gain |
| `phaseFunctions.singleScattering1/2` | §3.2 | forward HG lobes `(g,w)` |
| `phaseFunctions.multipleScattering1/2` | §3.2 | diffuse HG lobes `(g,w)` |
| `lightVolumeSettings` + `raymarchedClouds.cfg` | §3.3 | MS light volume, time slicing |
| `speed / detailSpeed / upwardsCloudSpeed` | §7 | motion |
| `droplets / particleFieldConfig / wetSurfacesConfig / lightningConfig` | §8 | downstream FX |

---

## 10. Porting to Asterra (Godot 4, `gl_compatibility` + Forward+)

What maps directly:

| EVE piece | Asterra equivalent | Action |
| --- | --- | --- |
| `coverageMap` / `cloudTypeMap` painted `.dds` | `WeatherSystem` global GPU weather texture (precip / humidity / cloud channels) | **the key change** — sample the live sim field instead of a static map so fronts and storms move clouds. `cloudTypeMap` ≈ a stability/CAPE-derived channel. |
| Scatterer `integrateWithEVEClouds` + aerial perspective | `shaders/atmosphere.gdshaderinc` (`aerial_perspective`, `ocean_view_transmittance`) | composite the cloud buffer before the atmosphere resolve; reuse the same transmittance. |
| `sunlightExtinction` | `CelestialSystem` sun + existing `planet_direct_sun_visibility` | multiply directional light by cloud transmittance sampled along the sun ray. |
| spherical noise, blue-noise jitter, TAA-friendly sampling | already in `ocean_geometry_clipmap.gdshader` (geodesic phase coords, STBN-style dithering) | lift the geodesic sampling helpers. |
| scaled-space `_MainTex` sphere | the orbital planet shader / `orbit_elevation` path | render a cheap 2D cloud sphere for `camera_alt > handoff`; cross-fade like the ocean's `u_orbit_handoff_altitude`. |
| light volume (224³-ish) | new: a small `ImageTexture3D` or a `RenderingDevice` compute-filled 3D buffer around the active body, refilled over N frames | needed for cheap in-cloud MS; keep it body-local like the hydrology atlas. |
| `particleField` / `droplets` / `wetSurfaces` | `GPUParticles3D` under coverage; screen-space droplet shader; a wetness channel on the terrain material | drive all three from the same `WeatherSystem` coverage/precip lookup + `minCoverageThreshold`. |

Suggested build order: (1) single raymarched shell sampling `WeatherSystem`
coverage, Beer–Lambert + one HG lobe, no MS; (2) detail erosion + coverage/
density curves per cloud type; (3) sun light-march + powder; (4) light volume
for MS; (5) atmosphere composite + god-rays + sun extinction; (6) scaled-space
handoff; (7) precip particles + wet surfaces. Tuning values to start from are in
[`README.md`](README.md) §"The look, distilled".
