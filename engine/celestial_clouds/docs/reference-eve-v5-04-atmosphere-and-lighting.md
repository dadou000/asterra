+++
path = "/rendering/clouds/reference-eve-v5-lighting"
title = "EVE V5 / Scatterer atmosphere and cloud light transport integration"
kind = "concept"
status = "proposed"
owner_module = "OrbitCelestialClouds"
summary = "Verified Scatterer coupling options and an independent, physically unit-consistent Orbit sunlight/skylight, multiple-scattering, shadow-volume and godray contract."
keywords = ["scatterer", "atmosphere", "optical", "sunlight", "sky", "multiple scattering", "light volume", "godrays", "eclipses"]
related = ["/rendering/clouds/reference-eve-v5", "/rendering/clouds/reference-eve-v5-medium"]
depends_on = ["/rendering/clouds/reference-eve-v5-medium"]
+++

# What the reference actually provides

**EXACT-ARCHIVE**:
- Bundled Scatterer config enables `integrateWithEVEClouds`, `sunlightExtinction`, `useEclipses`, `useRingShadows`, `useRaymarchedCloudGodrays`, and ocean sky/reflection/shadow integration, while `disableAmbientLight=True` in KSP's own ambient-light control. That flag does NOT prove all physically based sky lighting is disabled: cloud configs also explicitly request `skylightMultiplier` and `skylightTintMultiplier`.
- Kerbin Scatterer atmosphere profile names `HR`, `HM`, `m_betaR`, `BETA_MSca`, `m_mieG`, optional ozone scattering parameters, `multipleScattering`, `cloudColorMultiplier`, `cloudScatteringMultiplier`, `cloudSkyIrradianceMultiplier`, `volumetricsColorMultiplier`, ground reflectance, per-altitude sky/scattering exposures.
- Kerbin's example has `m_mieG=0.85`, multipleScattering True, cloudColorMultiplier=3, cloudScatteringMultiplier=0.2, cloudSkyIrradianceMultiplier=0.05, volumetricsColorMultiplier=1. Those are KSP/Scatterer-scoped tuning values, **not generic PBR albedos**. Import them into a *versioned reference calibration profile*, but do not globally change Orbit's radiometric light units or eye adaptation.
- Released cloud config includes 2 direct single-scattering and 2 multiple-scattering phase controls; *their exact shader equations and normalization are not revealed in the ZIP*.
- Reference light volume global settings: horizontalResolution=224, directLightTimeSlicing=12, maxTimewarpUpdateRateIncrease=3. Changelog claims long-distance cloud self-shadow / godrays, time-sliced cache and diffuse sky lighting; no compiled cache layout can be proven from the ZIP.

# Orbit authoritative light sources (verified symbols)

**EXISTING-ORBIT**:
- `celestial_atmosphere::AtmosphereRenderView` carries body-fixed camera and Sun, stellar irradianceScale, finite sun angular radius, actual ellipsoid axes and `terrainSunShadow` shared buffer.
- `celestial_atmosphere::AtmosphereRenderer::Draw` integrates atmosphere single+multiple scattering from the atmosphere top shell using GPU LUTs (`GpuAtmosphereLuts`).
- `celestial_atmosphere::SkySphericalHarmonics` and `EvaluateSkyIrradiance` provide a sky-radiance-to-irradiance projection in the correct sky frame; do not manually reuse a body-frame Sun vector in sky-frame basis.
- `lighting::DirectLightingRenderer::Draw` already accepts a cloudShadow texture interpreted as reduced-resolution direct solar transmittance in the red channel, optional proxy shadow, radiance cache, terrain Sun visibility and local lights.
- `lighting::TerrainSunShadowField` represents terrain horizon occlusion and GPU-uploaded query data; `SunVisibilityShader.hpp` and `AnalyticBodyVisibility.hpp` are the authoritative body/ellipsoid visibility.
- `optical_transport::StellarVisibility` and `CloudRepresentation` express shared optical and LOD concepts. These are physical authorities, not alternate renderer-specific Suns.

# Unified illumination contract (NEW)

Define a single immutable reference query per frame. A/B/reference backends consume the SAME physical sky, irradiance and eclipse metadata but own separate cloud optical integrations and cloud light caches.

```cpp
struct ReferenceLightingInput {
    universe::BodyId body;
    math::Double3 sunDirectionBody;  // from sample toward star
    math::Float3 starIrradianceSceneLinear;
    double angularSolarRadiusRadians;
    math::Double3 ellipsoidRadiiMeters;
    u64 bodyRevision, atmosphereRevision, sunRevision, terrainRevision;
    const celestial_atmosphere::GpuAtmosphereLuts* atmosphereLuts;
    std::shared_ptr<rhi::Buffer> terrainSunShadow;
    // Optional sky SH projected using the correct body<->sky frame basis.
    celestial_atmosphere::SkySphericalHarmonics skyRadiance;
};
struct ReferenceLightQuery {
    math::Double3 pointBody;
    math::Double3 viewDirectionBody;
    MediumCoefficients medium;
    double footprintMeters;
    bool allowCache;
};
struct ReferenceLightResult {
    math::Float3 sourceRadiancePerMeter;    // to integrate with sigmaS
    float cloudSunTau;                      // scalar, nonnegative
    float cloudSunTransmittance;            // exp(-tau)
    float planetSunVisibility;              // finite disc; 0..1
    float terrainSunVisibility;             // 0..1
    float atmosphereSunTransmittance;       // wavelength-dependent in final model
    float skyHemisphereAccess;              // 0..1
    bool valid;
};
ReferenceLightResult EvaluateReferenceLight(
    const CloudSceneSnapshot&, const ReferenceLightingInput&,
    const ReferenceLightQuery&, ReferenceLightCache&);
```

**Don't conflate** Sun optical depth, geometrical eclipse, terrain horizon, atmospheric extinction and diffuse sky accessibility. They are separate terms with independent cache invalidation.

# Direct solar transport

For a point P, the direct light term contains:
1. Solar spectral irradiance divided by Orbit's scene-reference irradiance, in the exact `AtmosphereRenderView` units.
2. Finite-disc visibility against the actual body ellipsoid and other eclipsing bodies; terrain horizon visibility for the local landscape (including mountains beyond screen space).
3. Atmospheric transmittance from P to the star supplied by authoritative atmosphere LUTs.
4. Cloud transmittance along P→Sun, from *reference backend's own* cloud optical-depth cache.
5. Scattering phase normalized to integrate to one over 4π sr for each single-scattering lobe.

```hlsl
float HG(float mu, float g) {
    float gg = g*g;
    float denom = max(1.0+gg-2.0*g*mu, 1e-5);
    return (1.0-gg) * (0.07957747154594767) *
           rsqrt(denom)*rsqrt(denom)*rsqrt(denom);
}
float3 DirectSource(
    float3 starIrradiance,
    float finiteDiscVisibility,
    float terrainVisibility,
    float3 airTransmittance,
    float cloudOpticalDepth,
    float scatteringAlbedo,
    float3 phaseFunction)
{
    const float vis=saturate(finiteDiscVisibility) *
        saturate(terrainVisibility) *
        exp(-max(cloudOpticalDepth,0.0));
    return starIrradiance*airTransmittance*phaseFunction*
           vis*scatteringAlbedo;
}
```

This is a baseline independent physical algorithm; it is not the unreleased V5 shader. Note that the cloud shader's sigmaS may already include albedo: pass either normalized albedo or sigmaS/sigmaT, never both. The atmosphere LUT source is in radiance/irradiance units and must not be multiplied by the reference's legacy Scatterer cloudColorMultiplier twice. Use one clearly named calibration stage after physical transport if striving for reference KSP artistic response.

## Disc/eclipse/terrain occlusion

For the star disc, a single centre ray causes a hard solar terminator. A finite-disc visibility fraction should integrate or approximate a disc overlap with the body's ellipsoid silhouette and terrain horizon angle. The available large-scale `AnalyticBodyVisibility` and TerrainSunShadowField should be queried at body-fixed sample positions. Scene depth is NOT a sufficient terrain occlusion proxy, since an offscreen mountain can block the Sun.

Avoid the old B problem: `upSun = saturate(0.5+0.5*dot(up,sun))` creates daylight when the star is behind the world. A scalar visibility from the authoritative stellar visibility service is required, with actual terrain blocker, proper penumbra and eclipse multiplicity. Cloud self-shadow must not multiply that same geometry blocker twice.

# Skylight and multiple scattering

**PUBLIC-SOURCE + EXACT-ARCHIVE** V5 documents a separately computed light volume, multiple-scattering phase controls and scattering of cloud skylight. Actual shader transfer function unknown.

Proposed transport:
- Use `ProjectSkyViewToSphericalHarmonics` on the *appropriate observer/Sun sky frame* to approximate incident directional sky radiance without re-raymarching the atmosphere at every cloud sample.
- For dense clouds, estimate visible sky hemisphere based on local cloud optical-depth/horizon queries sampled with a small independent directional set or a light-volume cache, not by reusing the Sun ray as an ambient proxy.
- Integrate first-order cloud scattering with normalized phase; add bounded energy-conserving higher-order response calibrated against the golden reference and without perpetual night glow.
- Local lights/lightning are separately distance/falloff-limited contributors. Their radiance source is the authoritative `LocalLightRegistry` / transient flash event, not an ad-hoc extra Sun.
- Validate Lambertian sky irradiance conservation: sky irradiance = integral(hemisphere L_sky(w) max(dot(n,w),0) dω). A physically correct sky SH contains directional coefficients, not a single constant.

**PROPOSED-PORT** equivalent sampling interface:

```cpp
struct SkyLighting {
    math::Float3 incidentIrradiance; // W/m2 normalized scene units
    float visibility;                 // 0..1
};
SkyLighting SampleCloudSky(
    const CloudSceneSnapshot& scene,
    const celestial_atmosphere::SkySphericalHarmonics& sky,
    const math::Double3& upBody,
    const math::Double3& sunDirectionBody,
    math::Double3 positionBody)
{
    const auto basis = celestial_atmosphere::MakeSkyFrameBasis(
        upBody, sunDirectionBody);
    const auto irradiance = celestial_atmosphere::EvaluateSkyIrradiance(
        sky,basis.FromBody(upBody));
    return {irradiance, scene.CloudSkyVisibility(positionBody)};
}
```

This shows the required code boundary; `CloudSkyVisibility` is a new cache query to be implemented, not an existing Orbit method. Its lighting must be independent of camera exposure and adaptation.

# Time-sliced Sun light volume

The reference's 224 horizontal resolution / 12 direct-light slices / ≤3 timewarp update multiplier are shipped settings. Implement a **separate**, per-view or per-region B/V5 light-volume product with validity masks and generations. Each voxel stores light optical depth, valid generation and footprint; optional directional sky openness and epoch. When a scene revision changes, invalidate *only* dependency-affected channels.

```cpp
struct ReferenceLightVolumeStamp {
    u64 bodyHigh, bodyLow, cloudRevision, terrainRevision;
    u64 stormRevision, atmosphereRevision, sunRevision;
    u64 lightingQualityRevision;
    i64 physicalTimeEpoch;
    u32 cascade, generation;
};
struct ReferenceLightVoxel {
    float tauSun;
    float skyVisibility;
    float confidence;
    float generationTag;
};
struct ReferenceCacheJob {
    ReferenceLightVolumeStamp stamp;
    u32 firstSlice, sliceCount;
    u32 maxDirectSteps;
    // Render graph owns a GPU fence pinned snapshot until completion.
};
```

Do not cap Sun rays at an arbitrary 6–12 samples uniformly over hundreds of kilometres. Split the **occupied cloud intervals**, use conservative transmittance accumulation, reduce detail only if filtered optical depth remains bounded. Cascades need overlapping transitions in optical-depth space, not an abrupt gain change. A local light cache must not be reused on another planet just because radii and texture fingerprints happen to match.

## God rays and clouds shadowing atmosphere

**EXACT-ARCHIVE** `useRaymarchedCloudGodrays=True`, `raymarchedGodraysStepCount=50`. The renderer must expose a shared *cloud solar transmittance query* at an atmosphere scattering point; this is not a separate cloud-shadow overlay darkening completed pixels. The atmosphere volume ray samples:
`dL_air = T_air(camera,P) * sigma_air(P) * L_sun(P) * T_cloud(P→Sun) * phase_air * ds`.
Cloud and air in-scattering must not be composited twice. The integrator may require scheduled alternation/fusion of atmosphere and clouds per scene-space segment rather than stacking two already integrated passes.

**PROPOSED-PORT**:
```hlsl
float3 IntegrateGodraySegment(float3 rayStart, float3 rayDir,
                             float t0, float t1, uint steps)
{
    float3 L=0., T=float3(1,1,1);
    float ds=(t1-t0)/max(steps,1u);
    [loop] for(uint i=0;i<steps;++i) {
        float3 p=rayStart+rayDir*(t0+(i+0.5)*ds);
        AtmosphereSample air=SampleAtmosphere(p);
        float cloudT=GetCloudSunVisibility(p); // valid independent cache
        float sunV=GetPlanetTerrainSunVisibility(p);
        float3 dL=air.sigmaScatter*air.phase*
                  air.starRadiance*sunV*cloudT;
        L += T*dL*ds;
        T *= exp(-air.sigmaExtinction*ds);
    }
    return L;
}
```

## Reflections, glass, water and emissive

The cloud reference produces a coarse stable sky radiance and transmittance descriptor for water/glass/probes. Do not execute its full 128+ view-ray samples on each reflected pixel. Choose a reflection-cloud LOD using Orbit's `optical_transport::CloudRepresentation`, then feed sky/GI/cache through the existing reflection and material interfaces. Sun shadows are not an arbitrary post-color darkening. Distant mirrored clouds still have location/time consistency. If glass has expensive refraction, use a cloud representation budget and stable alpha/reconstruction.

# Accuracy and performance tests

- Noon, 5° solar altitude, 0° horizon, just below horizon, high clouds extending above geometric horizon, eclipses and mountain-occluded Sun.
- Compare direct Sun visibility at a surface and at nearby cloud point. Receiver shadow and volumetric light must agree about the same cloud column, with different spatial integration resolution allowed.
- Check sky SH frame orientation at poles and near-zenith Sun; no flip of cloud shade when camera rotates.
- Toggle cloud and terrain god rays independently, ensuring terrain godrays flag is false in default shipped profile.
- Validate T_cloud=exp(-tau) under dense overlapping layers and secondary light integration, no negative sigma or NaN.
- No bright night clouds without lightning/local emissive lighting or legitimate reflected illumination.
- Profile Sun-volume warm/cold, p95 time, memory, cache invalidation, low-angle ray length; timewarp updates cannot flood GPU.
