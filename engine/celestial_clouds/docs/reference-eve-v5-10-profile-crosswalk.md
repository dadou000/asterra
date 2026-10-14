+++
path = "/rendering/clouds/reference-eve-v5-profile-crosswalk"
title = "EVE V5 per-planet cloud, Scatterer atmosphere and ocean parameter crosswalk"
kind = "concept"
status = "proposed"
owner_module = "OrbitCelestialClouds"
summary = "Verified ordered cloud-type names/altitudes and per-planet Scatterer atmospheric parameters, with import/physical-unit and test semantics."
keywords = ["Kerbin", "Laythe", "Eve", "Duna", "Jool", "cloud type", "atmosphere", "ocean", "scatterer", "reference profile"]
related = ["/rendering/clouds/reference-eve-v5", "/rendering/clouds/reference-eve-v5-package"]
depends_on = ["/rendering/clouds/reference-eve-v5-config"]
+++

# Scope

**EXACT-ARCHIVE** structured data extracted from the supplied August 2026 preview's `StockVolumetricClouds/Clouds/clouds.cfg` and `StockScattererConfigs/Planets/*/atmo.cfg` / ocean.cfg. This is an index of numeric/name settings, **not** a copy of proprietary maps or shader source. The full private config remains the authoritative source of all 293 Hermite control-point lines, type densities/tilings, painter remaps, wind and effect links; use `tools/audit_eve_reference.py` and the typed importer, not hand-copying abbreviated matrices. A physical-unit meaning is not assumed solely from a source parameter's name.

# Exactly ordered cloud-type index

The cloudTypeMap scalar interpolates the **source order** of the listed Items. Preserve order; never group alphabetically, and never infer a physical type from its string alone. Altitude numbers below are authored metres. A question mark denotes a missing explicit minimum altitude, not zero.

| Body | V5 layer | Ordered type names with [base..top] metres |
|---|---|---|
| Kerbin | Kerbin-base-layer | Stratus [1750..3450]; Cumulus [1750..4450]; Congestus0 [1750..5450]; Congestus 1 [1750..6450]; Congestus 2 [1750..7450]; Congestus 3 [1750..7450] |
| Kerbin | Kerbin-cumulonimbus-layer | trail [1750..11450]; edge [1750..11450]; core [1750..11450] |
| Kerbin | Kerbin-Weather-1 | Fog [-20..2150]; Rain [-20..2150]; Thunder [-20..2150]; Polar-fog [-20..2150]; Polar-snow [-20..2150] |
| Kerbin | Kerbin-weather-2 | Fog [-20..2150]; Rain [-20..2150]; Thunder [-20..2150]; Polar-fog [-20..2150]; Polar-snow [-20..2150] |
| Kerbin | Kerbin-Snow-Particles-1 | SnowParticles [-1..2150] |
| Kerbin | Kerbin-Snow-Particles-2 | SnowParticles [-1..2150] |
| Duna | Duna-dust-scattered | Dust [-20..8500] |
| Duna | Duna-duststorm-big | Wake [?..10500]; Front [?..9000]; Inverted Tornado bottom [?..8000]; Inverted Tornado top [?..10500]; Regular Tornado bottom [0000..8000]; Regular Tornado top [4000..9000] |
| Duna | Duna-rare-cirrus | Cirrus [12000..12500]; Cirrus2 [12000..13500] |
| Eve | Eve-clouds1 | Floor [2500..12500]; Pillar [2500..8500] |
| Eve | Eve-clouds2 | Stratus [12500..14500]; New cloud type 1 [12500..15500]; New cloud type 2 [12500..15000] |
| Jool | Jool-clouds-underworld | Floor [-80000..-20000]; Pillar [-80000..-21000] |
| Jool | Jool-clouds0 | Floor [-20000..20500]; Pillar [-20000..20500] |
| Jool | Jool-clouds1 | Swirl [20000..25000]; Low [20000..50000]; High [20000..60000]; High2 [20000..70000] |
| Jool | Jool-clouds2 | Classic Cumulonimbus top [70000..110000]; Classic Cumulonimbus core [70000..110000]; Hurricane top [50000..110000]; Hurricane core [50000..110000]; Billow soft edge [70000..77000]; Billow Core [70000..110000]; Cumulonimbus top 4 [70000..110000]; Cumulonimbus core 4 [70000..110000] |
| Laythe | Laythe-base-layer | Stratus [1750..2940]; Cumulus [1750..3640]; Congestus0 [1750..4340]; Congestus 1 [1750..5040]; Congestus 2 [1750..5740]; Congestus 3 [1750..5740] |
| Laythe | Laythe-pyrocumulus-layer | trail [2500..14200]; top [2500..14200]; bottom [2500..14200] |
| Laythe | Laythe-Weather-1 | Fog [-20..2150]; Rain [-20..2150]; Thunder [-20..2150]; Polar-fog [-20..2150]; Polar-snow [-20..2150] |
| Laythe | Laythe-weather-2 | Fog [-20..2150]; Rain [-20..2150]; Thunder [-20..2150]; Polar-fog [-20..2150]; Polar-snow [-20..2150] |

**EXACT-ARCHIVE** overlapOrder applies to whole layers (e.g., main Kerbin cumulonimbus 2, weather/fog 3). Jool negative altitudes require deep-atmosphere optical cutoffs instead of a conventional solid terrain shadow at sea level. SnowParticles layers explicitly disable cloud-volume march via fxOnlyLayer, while particle events remain active.

# Scatterer atmospheric controls

These are numerical authored *reference sliders*, not yet SI-normalized atmospheric coefficients. Keep original values, fields, and physical-unit conversion metadata in the importer. HR and HM are named atmospheric scale heights but the ZIP alone does not establish all KSP conversion and LUT scale factors.

| Body | HR | HM | Mie g | Rayleigh coefficient m_betaR | Mie coefficient BETA_MSca |
|---|---:|---:|---:|---|---|
| Kerbin | 3.84 | 1.0 | 0.85 | (0.015341,0.0357075,0.0875489) | (0.0121406,0.0121406,0.0121406) |
| Duna | 5.0 | 3.0 | 0.70 | (0.04485,0.0225,0.0078) | (0.0045,0.009,0.009) |
| Eve | 8.0 | 4.2 | 0.82 | (0.078623,0.029649,0.078623) | (0.016,0.016,0.016) |
| Jool | 46.40625 | 5.56875 | 0.85 | (0.002844,0.008425,0.002313) | (0.005782,0.005782,0.005782) |
| Laythe | 3.75 | 1.0 | 0.55 | (0.007734,0.018,0.057466) | (0.1024,0.1024,0.1024) |

All five listed atmosphere configs set multipleScattering=True; default KSP cloudScatteringMultiplier is 0.2 in these configs, but absolute radiance calibration varies by atmosphere body and per-altitude configPoints. Do NOT use the same eye-exposure output transform for arbitrary Scatterer tuning values without calibrated semantics.

| Body | cloudColorMultiplier | cloudSkyIrradianceMultiplier | volumetricsColorMultiplier | godrayStrength | ozone |
|---|---:|---:|---:|---:|---|
| Kerbin | 3 | 0.05 | 1 | 0.7 | on; ozone height 20 |
| Duna | 1 | 0 | 1 | 0.8 | source config not enabled |
| Eve | 1 | 0 | 3 | 0.8 | source config not enabled |
| Jool | 1 | 0 | 1 | 0.8 | source config not enabled |
| Laythe | 3 | 0 | 1 | 0.8 | on; ozone height 0 |

These configurations reveal actual *differences*, not a universal cloud lighting formula. The cloud renderer must query the authoritative atmosphere per planet rather than baking Kerbin tint into all cloud shaders. The source includes optional ozoneAbsorption/falloff, noonSunlightExtinctionStrength, sky/scattering exposure, configPoints and scaled-land/ocean contrast/saturation adjustments, which the typed importer must preserve even when not yet supported.

# Ocean configuration crosswalk

Eve, Kerbin and Laythe have separate ocean.cfg files. Their `AMP` values are Eve 0.05, Kerbin 1, Laythe 1. In KSP these are an ocean spectral amplitude-like setting; **do not import them as a hard ocean elevation parameter**. Their additional spectrum/wind/wave and reflection/refraction parameters remain in the private config. The physical water pipeline of Orbit owns oceans and rivers, and consumes sky/cloud radiance/solar transmittance from this renderer, not the KSP ocean mesh.

# Calibration ownership

```cpp
// PROPOSED-PORT: imported per-planet numeric values stay unconverted
// until a documented source-domain->Orbit-domain conversion is validated.
struct ImportedScattererAtmosphere {
    std::string bodyName;
    double hrOriginal, hmOriginal, mieGOriginal;
    math::Double3 betaROriginal, betaMOriginal;
    double cloudColorMultiplierOriginal;
    double cloudScatteringMultiplierOriginal;
    double cloudSkyIrradianceMultiplierOriginal;
    double volumetricsColorMultiplierOriginal;
    double godrayStrengthOriginal;
    std::optional<math::Double3> ozoneAbsorptionOriginal;
    std::vector<ConfigNode> configPoints;
    u64 configChecksum;
};
struct PhysicalAtmosphereMapping {
    celestial_atmosphere::AtmosphereParameters parameters;
    u64 transformVersion;
    std::vector<Diagnostic> conversionWarnings;
};
PhysicalAtmosphereMapping MapScattererToOrbit(
    const ImportedScattererAtmosphere& source,
    const universe::BodyShape& shape,
    const CelestialLightingReference& star,
    const AtmosphereConversionCalibration& calibration);
```

Before implementing `MapScattererToOrbit`, inspect the current relevant Scatterer public source for coefficient units and body radius scaling, then calibrate against a licensed running KSP reference. A body with missing atmosphere cannot be made "equivalent" by reusing Kerbin LUTs. Shared optical transport should consume the mapped Orbit `AtmosphereParameters`; no parallel Scatterer atmosphere physical solver is instantiated inside the cloud renderer.

# Required calibration captures and tests

1. Transmittance at zenith and at 0°/5° solar altitude for each body's equivalent atmosphere and configured clouds.
2. Sun and sky colour with clouds disabled, then Cloud-only delta, to separate atmospheric response from cloud scattering multiplier.
3. Ozone on/off for Kerbin/Laythe; never double ozone attenuation.
4. Same cloud type scalar at source-ordered bounds 0/1 and between adjacent types; compare actual V5 coverage/density Hermite curves.
5. Negative gas-giant altitudes and upper clouds across orbital transition, no invented terrain clip.
6. Source ocean amplitude, planet reflection and cloud receiver transmittance tested independently.
7. Reference config hash included in screenshot/profiler sidecar; artistic multipliers only applied in a separately named calibration stage.

# Missing texture dependencies

The exact order/numeric heights do not define the physical maps. The supplied ZIP omits 103 logical cloud assets (76 textures, 27 sounds). These are required to reproduce cloud locations, morphology, flow and FX. A synthetic map can test imported object schema, but is NOT a verified reference reproduction.
