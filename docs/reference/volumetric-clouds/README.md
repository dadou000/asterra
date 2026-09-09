# Volumetric cloud reference — Blackrack Raymarched Volumetrics (EVE)

Reference material for building Asterra's own volumetric cloud renderer. The
goal is to reproduce the **look** of Blackrack's Raymarched Volumetric Clouds
first, then improve on it with Asterra's own weather simulation driving coverage
instead of static painted maps.

## Provenance & licensing

- Source: *Raymarched Volumetrics — Early Access* (`30_09_25` build), a **paid**
  add-on for Kerbal Space Program by Blackrack, bundling Environmental Visual
  Enhancements (EVE), Scatterer, and the Stock Volumetric Clouds config pack.
- This directory is **reference only**. Do not ship any of it in an Asterra
  build and do not redistribute it. Only the small text configs and the generic
  noise primitives are copied here; the mod's DLLs, Unity shader bundles,
  per-planet scaled-space imagery, and audio (~1.4 GB) are intentionally left
  out.
- The engine is Unity/HLSL; none of it runs in Godot. Treat the `.cfg` files as
  a parameter spec and the `Documentation.txt` as the design rationale.

## What's here

```
GameData/StockVolumetricClouds/raymarchedClouds.cfg  quality / light-volume settings
GameData/StockVolumetricClouds/Clouds/clouds.cfg     the layer + cloud-type definitions (main reference)
GameData/StockVolumetricClouds/shadows.cfg           cloud shadow casting
GameData/StockVolumetricClouds/pqs.cfg               terrain-scale fade
GameData/StockVolumetricClouds/Clouds/droplets.cfg   near-camera droplet/precip particles
GameData/StockVolumetricClouds/Clouds/lightning.cfg  storm lightning
GameData/StockVolumetricClouds/Clouds/wetSurfaces.cfg surface wetness response
GameData/Scatterer/config/*                          atmosphere / aerial-perspective tuning
GameData/StockScattererConfigs/*                     per-body atmo + ocean scattering
GameData/EnvironmentalVisualEnhancements/Documentation.txt

textures/detail1.dds            3D detail (erosion) noise volume
textures/uvnoise1.dds           2D UV-warp noise
textures/stbn.R8                spatiotemporal blue noise (scalar) — raymarch jitter
textures/stbn_unitvec3.ARGB32   spatiotemporal blue noise (unit vectors) — light-march jitter
```

Deliberately omitted (grab from the Desktop copy if a specific one is needed):
`*.dll`, `eveshaders.bundle`, `scatterershaders-{opengl,directx}`, every
`Textures/PluginData/<Planet>/{base,top}/{scaled,normals}.dds` (the 179 MB
scaled-space maps), coverage/type maps, `*.sdf`, `Sounds/`.

## The look, distilled

From `clouds.cfg` → `layerRaymarchedVolumeV5` (Kerbin base layer, the Earth-like
preset):

| Aspect | Value | Notes |
| --- | --- | --- |
| Layer altitude | 2200 m base, cloud tops to ~4450 m | `cloudTypes` min/maxAltitude per type |
| Raymarch base step | 75 m, adaptive factor 0.006, max 1500 m | cone-widening steps with distance |
| Light march | 4 steps, 1200 m total | cheap forward-scatter transmittance |
| Detail noise | `detail1`, scale 30, Worley, persistence 0.57, erosion depth 0.65 | `spherical = 1` |
| UV warp | `uvnoise1`, strength 2.0e-4 | breaks up tiling |
| Phase functions | SS `(0.95,0.1)` + `(0.8,0.2)`; MS `(0.2,3.0)` + `(-0.4,0.3)` | dual-lobe HG, forward + back |
| Skylight | multiplier 1.0, tint multiplier 0.7 | ambient fill from sky |
| Scaled fade | start 200 km, end 260 km | hand-off to painted scaled-space imagery |
| Cloud types | Stratus (density 0.0015), Cumulus (density 0.012) | per-type coverage/density curves, `noiseEdgeHardness` 0.9–0.97 |
| Colour | layer `120,120,120`; `_Lambertian = 0.75` | |

Coverage and cloud-type are painted `AlphaMap` textures (`ALPHAMAP_R`) in the
mod. **Asterra's intended change:** feed those two channels from
`WeatherSystem`'s live coverage/precipitation fields instead of static maps, so
fronts and storms move the clouds.

`Scatterer/config/config.cfg` and the per-body `atmo.cfg` files are the
atmosphere model the clouds are lit against — cross-check against Asterra's
`atmosphere.gdshaderinc` when matching in-scatter colour and aerial perspective.
