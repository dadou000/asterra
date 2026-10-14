+++
path = "/rendering/clouds/reference-eve-v5-package"
title = "EVE V5 release inventory and exact parameter families"
kind = "concept"
status = "proposed"
owner_module = "OrbitCelestialClouds"
summary = "Verified ZIP inventory, all 19 volumetric layers, 70 type entries, quality and cross-system configuration and missing dependencies."
keywords = ["EVE V5", "reference inventory", "quality presets", "cloud types", "config", "asset inventory"]
related = ["/rendering/clouds/reference-eve-v5"]
depends_on = ["/rendering/clouds/reference-eve-v5"]
+++

# Release identity and artifact policy

**EXACT-ARCHIVE**: user-provided RaymarchedVolumetricsEarlyAccess23_08_26(1).zip, SHA-256 `d4c74123c76651969bc81073e3e6eb2e8236ae464a336618fd9baf8baebc42a7`. 87 ZIP entries, 65.37 MB compressed, ~66.73 MB expanded. Date of final changelog entry: 23/08/26. Do not commit bundle, DLL/PDB, STBN, sound or licensed textures to this repository.

Top-level actual packages:
- GameData/EnvironmentalVisualEnhancements: eveshaders.bundle; Plugins/Atmosphere, CelestialShadows, CityLights, EVEManager, PartFX, PQSManager, ShaderLoader, Terrain, TextureConfig, Utils and _BuildManager DLL/PDB; stbn.R8; stbn_unitvec3.ARGB32.
- GameData/Scatterer: Scatterer.dll, scatterer.pdb, shaders/scatterershaders-directx and scatterershaders-opengl, STBN, global and preset cfgs.
- GameData/StockScattererConfigs: per-planet atmosphere/ocean configs (Duna/Eve/Jool/Kerbin/Laythe), planet list, sunflare and related images.
- GameData/StockVolumetricClouds: clouds.cfg, lightning.cfg, particleFields.cfg, droplets.cfg, wetSurfaces.cfg, pqs.cfg, raymarchedClouds.cfg, shadows.cfg. Cloud maps, painter maps, particle textures and effect audio are **not** included.
- GameData/Blackrack_TUFX.cfg is an optional postprocessing profile. Deferred, TUFX and Parallax are described in the readme as optional external components (Deferred recommended for SSR/puddles/deferred lighting).

# Exact cloud-layer inventory from shipped V5 configuration

Every row below is a separate EVE_CLOUDS / OBJECT with layerRaymarchedVolumeV5. Numbers are layer counts of cloudTypes and painterTiles. Heights are metres from the authored config, NOT Orbit's assumed cloud layer range.

| Planet | Layer name | Types | Painter tiles | Min/max example cloud-top ranges |
|---|---|---:|---:|---|
| Kerbin | Kerbin-base-layer | 6 | 13 | Stratus 1750–3450; Cumulus 1750–4450; Congestus up to 7450 |
| Kerbin | Kerbin-cumulonimbus-layer | 3 | 1 | trail, edge, core 1750–11450 |
| Kerbin | Kerbin-Weather-1 | 5 | 0 | Fog/Rain/Thunder/Polar-fog/Polar-snow -20–2150 |
| Kerbin | Kerbin-weather-2 | 5 | 0 | Same categories -20–2150 |
| Kerbin | Kerbin-Snow-Particles-1 | 1 | 0 | FX-only layer -1–2150 |
| Kerbin | Kerbin-Snow-Particles-2 | 1 | 0 | FX-only layer -1–2150 |
| Duna | Duna-dust-scattered | 1 | 0 | Dust -20–8500 |
| Duna | Duna-duststorm-big | 6 | 0 | Wake/front and inverted/regular tornado types, tops 8000–10500 |
| Duna | Duna-rare-cirrus | 2 | 0 | Cirrus 12000–12500 / 13500 |
| Eve | Eve-clouds1 | 2 | 0 | Floor 2500–12500; Pillar 2500–8500 |
| Eve | Eve-clouds2 | 3 | 0 | Stratus and two cloud types 12500–15500 |
| Jool | Jool-clouds-underworld | 2 | 0 | Floor/Pillar -80000 to about -20000 |
| Jool | Jool-clouds0 | 2 | 0 | Floor/Pillar -20000–20500 |
| Jool | Jool-clouds1 | 4 | 0 | Swirl/Low/High/High2 20000–70000 |
| Jool | Jool-clouds2 | 8 | 0 | Cumulonimbus/Hurricane/Billow variants 50000–110000 |
| Laythe | Laythe-base-layer | 6 | 13 | Stratus 1750–2940; Cumulus 1750–3640; Congestus up to 5740 |
| Laythe | Laythe-pyrocumulus-layer | 3 | 0 | trail/top/bottom 2500–14200 |
| Laythe | Laythe-Weather-1 | 5 | 0 | Fog/Rain/Thunder/Polar-fog/Polar-snow -20–2150 |
| Laythe | Laythe-weather-2 | 5 | 0 | Same categories -20–2150 |

Total 19 layers, 70 type entries, 27 painter tiles. Some config authors omit minAltitude for individual Duna types: do not invent it in the importer; preserve absence and resolve only from an explicitly documented V5 schema default after obtaining source confirmation.

Important **EXACT-ARCHIVE** numeric parameters:
- Kerbin base: ray lightMarchSteps=4, lightMarchDistance=1200 m, baseStepSize=75 m, adaptiveStepSizeFactor=0.006, maxStepSize=1500 m; cloud noise erosionDepth=0.65, Worley spherical=1, persistence=0.57; vertical noise speed=5; sky multiplier 1.0, tint multiplier 0.7. Scaled fade 200000–260000 m.
- Kerbin main stratus: altitude 1750–3450, density 0.0015, noise tiling 2300; cumulus: 1750–4450, density 0.012, tiling 2300. Other congestus types reach 5450, 6450 or 7450 with type-specific nontrivial Hermite curves.
- Kerbin cumulonimbus overlapRenderOrder=2; weather/fog/rain layers overlapRenderOrder=3. **Overlap sorting is a first-class fidelity requirement**, not optional metadata.
- Kerbin fx-only snow layers use fxOnlyLayer=True and 90,000,000-m step size to avoid cloud volume rendering while retaining particle logic.
- Jool's deeper gas envelope uses negative heights to -80000 m, with ray settings as coarse as 750–3000 m and continuousAccumulationDistance=1000000. It cannot use the same spherical surface clipping as a solid planet.
- Jool scaled fade 3000000–5300000 m, Duna 1000000–1300000 m, Eve 100000–130000 m, Laythe 200000–260000 m.
- V5 authored phaseFunctions can contain singleScattering1/2 and multipleScattering1/2; these are user-visible controls, NOT a fully disclosed source shader equation.

# Nested configuration taxonomy

Actual top-level config roots: EVE_CLOUDS, EVE_LIGHTNING_CONFIG, EVE_PARTICLE_FIELD_CONFIG, EVE_DROPLETS_CONFIG, EVE_WET_SURFACES_CONFIG, EVE_RAYMARCHED_CLOUDS_QUALITY, EVE_SHADOWS, PQS_MANAGER, Scatterer_config, Scatterer_quality_preset, Scatterer_atmosphere, Scatterer_ocean, Scatterer_planetsList.

Within 19 cloud layers: 19 settings, 19 raymarchingSettings, 19 noise, 19 lightVolumeSettings, 19 phaseFunctions, 19 cloudTypes; 70 coverageCurve and 70 densityCurve sections; 46 coverageMap, 31 cloudTypeMap, 4 cloudColorMap; 5 flowMap; 3 curlNoise; 3 painterTiles; 2 painterTileMasks; 5 lightning links; 7 particleField links; 5 droplet links; 2 wetSurfaces links; 9 timeSettings; 10 layer2D; 12 ambientSound. The presence of a node with no keys may still represent an enabled feature or a default profile; **do not interpret empty sections as zero values**.

Each curve consists of keys with up to four floats (time, value, incoming tangent, outgoing tangent). Preserve input order and tangent semantics; do not approximate these as linear control points.

# Reference tuning / quality profile

StockVolumetricClouds/raymarchedClouds.cfg sets:
- lightVolumeSettings.horizontalResolution = 224
- lightVolumeSettings.directLightTimeSlicing = 12
- lightVolumeSettings.maxTimewarpUpdateRateIncrease = 3

Scatterer config sets useRaymarchedCloudGodrays=True; useRaymarchedTerrainGodrays=False; raymarchedGodraysStepCount=50; useTemporalAntiAliasing=True; taaStationaryBlending=0.75, taaMotionBlending=0.65, taaJitterSpread=0.8, taaSharpness=0.28; disableTaaBelowFrameRateThreshold=26. These are actual shipped options, but whether Unity's underlying shader interprets a setting in a given way requires public source/runtime verification.

Scatterer quality presets: Integrated Graphics and Very Low disable most expensive light/ocean features; Low enables TAA but disables cloud godrays; Optimal turns on cloud godrays and ocean shader; High and Very High enable terrain godrays/shadows and increase long-range shadow budgets (e.g., unifiedCamShadowsDistance=25000 and 50000 m respectively). Orbit must use its own RHI/runtime quality admission, but map these settings into named fidelity profiles for controlled reference comparisons.

# Changelog-derived architectural requirements (observed shipped claims)

Release 5 (3.0.0 and later) introduces improved noise erosion/persistence/lacunarity/spherical cellular shape; removes former separate detail noise; updated de-tiling; continuous distance-aware temporal absorption, texture painting, density gradient curves, multiple scattering profile control and multi-layer overlap order. The 2026 updates specifically address temporal blending across thin/overlapping layers, fast camera motion, flow-map singularities, scaled-space depth, TAA in orbit, puddle shifting, ocean/stock-ocean overlap, cloud fading in map view and editor curves. These are evidence of required behaviors, **not proof of particular source algorithms**.

## Known missing inputs, fail-closed behavior

The 103 distinct unresolved paths (76 texture and 27 sound logical paths, including spaces within proprietary sound filenames) include Kerbin base/top coverage/type/scaled/normal maps and painter stamps, Duna duststorm flow and SDF, Jool flow/type/SDF, droplets noise, particle sheets, splash atlas and ambience audio. The actual map contents determine where each cloud appears. No generated Perlin map may be represented as "the exact V5 reference" without its original pixels. The importer must report (layer, role, logical path, missing/resolved, checksum, format, channel swizzle and dimensionality). No copy of the shipped configs or binaries is stored in this public repo by this documentation pass.

# Exactness verification checklist

1. Acquire legal access to the complete source and texture package, or retain the private original only on the user's machine for rendering a golden reference.
2. Validate layer and type counts and required fields before starting.
3. Use exact node names (layerRaymarchedVolumeV5, overlapRenderOrder, fxOnlyLayer) and preserve every optional value as optional rather than dropping it.
4. Hash imported maps and tie captures to those hashes; reject channel/face mismatches.
5. Compare 2D/orbit and 3D/local representations using matching config and lighting.
6. Map every V5 setting to a typed Orbit field or classify it explicitly UNSUPPORTED, rather than discarding unknown fields.
