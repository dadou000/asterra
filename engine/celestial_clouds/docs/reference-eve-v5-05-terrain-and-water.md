+++
path = "/rendering/clouds/reference-eve-v5-terrain"
title = "EVE V5 terrain, ocean, shadows, wetness and translucent surfaces"
kind = "concept"
status = "proposed"
owner_module = "OrbitCelestialClouds"
summary = "Code-level contract for terrain/ship/cloud occlusion, PQS compatibility, oceans, deferred wetness/puddles, reflection and depth."
keywords = ["terrain", "PQS", "cloud shadow", "ocean", "water", "wetness", "rain", "depth", "SSR", "reflection"]
related = ["/rendering/clouds/reference-eve-v5"]
depends_on = ["/rendering/clouds/reference-eve-v5-lighting"]
+++

# Why terrain and ocean belong in the cloud integration

**EXACT-ARCHIVE** cloud shadows affect terrain/ocean and deferred objects; Scatterer ocean receives cloud shadows, sky reflections, refraction, caustics and water interaction; precipitation drives puddle/wetness on distinct terrain, scenery and craft materials. The release changelog explicitly mentions fixes for stock-ocean mesh showing through clouds, ocean double rendering/marbling, cloud shadow alignment under camera zoom, cloud particles through terrain and puddles sliding during vehicle movement. The reference includes special gas-giant PQS kill-sphere options.

**EXISTING-ORBIT** has TerrainSource::Sample(TerrainQuery), TerrainSunShadowField, DirectLightingRenderer's cloudShadow texture, surface-buffer G-buffers, GI/proxy visibility and viewport depth. Clouds must use these native physical authorities, not import KSP's PQS terrain mesh or spawn a second ocean renderer.

## Physical frame and terrain samples

Terrain queries are planet-fixed and thread-safe. `TerrainQuery` includes unitDirection, footprintMeters, planet ID and radialOffsetMeters. Terrain edits increment TerrainSource::GenerationRevisions. Import world/planet identity into every cloud receiver query; don't treat a texture cube pixel as a terrain sample.

```cpp
struct SurfaceReceiver {
    world::PlanetId planet;
    math::Double3 pointBodyMeters;
    math::Double3 normalBody;
    math::Float3 baseColor;
    double footprintMeters;
    u64 terrainRevision;
};
std::optional<SurfaceReceiver> ResolveCloudShadowReceiver(
    const terrain::TerrainSource& terrain,
    const world::PlanetId planet,
    math::Double3 bodyFixedPoint, double radiusMeters,
    double footprintMeters)
{
    const double length=Length(bodyFixedPoint);
    if(!(length>0.0)) return std::nullopt;
    const math::Double3 direction=bodyFixedPoint/length;
    const terrain::TerrainQuery q{
        .unitDirection=direction,
        .footprintMeters=footprintMeters,
        .planet=planet,
        .radialOffsetMeters=length-radiusMeters
    };
    const auto sample=terrain.Sample(q);
    // Adapt the returned TerrainSample's canonical height/normal fields
    // via a dedicated terrain adapter; do not invent implicit height units.
    return MakeSurfaceReceiver(sample,q,terrain.GenerationRevisions());
}
```

`MakeSurfaceReceiver` is a new, explicitly named adapter, not an existing engine symbol. It must be implemented using verified TerrainSample fields and ellipsoid geometry. Only terrain/physics owns surface geometry; clouds may query it but not rewrite source height or generate a second PQS terrain model.

## Opaque visibility and depth

A cloud ray is clipped by the first actually opaque depth: terrain, authored mesh, ship/vehicle hull, building and planetary ellipsoid (as appropriate). Optical sky rendering should not be clipped by a *clear* reverse-Z depth buffer as though an infinitely distant surface existed. Depth reconstruction uses Orbit's camera projection and same floating-origin frame as the cloud ray.

**PROPOSED-PORT** GPU contract:

```hlsl
struct CloudDepthInput {
    float sceneDepthReverseZ;
    float nearPlaneMeters, farPlaneMeters;
    float3 rayDirectionCamera;
    float3 cameraOriginBodyRelativeMeters;
    float linearDepthMeters;
    uint depthValid;
};
float StopCloudRay(float cloudExit, CloudDepthInput input)
{
    if (input.depthValid==0u) return cloudExit;
    return min(cloudExit, max(0.0,input.linearDepthMeters));
}
```

The linear depth must be measured **along the cloud ray**; forward camera-Z depth cannot be substituted without dividing by cosine of the camera-ray angle. At oblique horizons this causes major cutoff lines. Use geometric body intersection as a separate visibility condition. Transparent glass and water require interface-specific handling, not pretending everything in scene depth is opaque.

Terrain mesh LOD updates, toroidal clipmap recentering and origin shifts must not move cloud shadows; compute light rays in stable body-fixed coordinates. This directly prevents “cloud shadow sliding” on zoom/origin updates. Invalidate receiver caches on terrain revision and cloud/weather/storm inputs, not every camera drift.

## Cloud receiver transmittance and the direct light pass

**EXISTING-ORBIT** DirectLightingRenderer::Draw has optional `rhi::Texture* cloudShadow`. Documentation specifies RED = Sun transmittance; multiplies the **direct stellar term only**. B/A already create reduced-resolution ShadowTarget and run receiver shadow passes in `StudioViewportLightingPass.cpp`.

**PROPOSED-PORT** V5 separate receiver light product:

```cpp
struct ReferenceCloudShadowTarget {
    std::unique_ptr<rhi::Texture> transmission; // R16F or RGBA16F fallback
    u32 width, height;
    u64 generation;
};
struct ReceiverSunQuery {
    math::Double3 bodyPointMeters;
    math::Double3 sunDirectionBody;
    float solarAngularRadius;
    u32 visibilitySamples;
};
float CloudReceiverSunTransmittance(
    const CloudSceneSnapshot& scene, const ReceiverSunQuery& query)
{
    // Integrate EXACT SAME cloud extinction field as view ray.
    const auto intervals=SceneCloudIntervalsToSun(scene,query.bodyPointMeters,
                                                  query.sunDirectionBody);
    const double tau=IntegrateCloudExtinction(scene,intervals,
                      query.sunDirectionBody,query.visibilitySamples);
    return static_cast<float>(std::exp(-std::max(0.0,tau)));
}
```

Both helper functions are original proposed functions; they must use the medium and geometry chapter's exact semantics, with distinct finite-disc and terrain visibility in the direct-light consumer. Never multiply A and V5 cloud-shadow maps together. Reprojection/downsample must preserve penumbra thickness, real-world light direction and cloud edge uncertainty.

## Atmosphere / ocean composite boundaries

For a water pixel, the physical path is camera→air→cloud volumes→water interface→refracted/reflected world. Orbit's atmosphere pass currently composites atmosphere before cloud passes; the reference integrator must prevent air scattering being added again through the same path. Define separate:
- Radiance and transmittance of cloud between camera and first interface.
- Aerial atmospheric path before interface (could be a fused integrator or precomputed atmosphere transmittance).
- Water interface BRDF, reflected sky/cloud radiance, refracted depth and volumetric extinction underwater.
- Water/cloud shadow applied to direct stellar irradiance at the water interface, not the final tonemapped ocean colour.
- Reflection products at coarse cloud LOD (cube/probe or analytical lookup), not a full high-budget second per-pixel march.

**EXACT-ARCHIVE** Scatterer config: useOceanShaders, oceanFoam, oceanTransparencyAndRefractions, shadowsOnOcean, oceanSkyReflections, oceanScreenSpaceReflections, oceanCaustics, oceanLightRays, oceanCraftWaveInteractions, oceanPixelLights, and separate quality toggles. These flags are a reference feature checklist, **not** a request to replace Orbit's ocean physics. The engine's fixed ocean level and dynamic river systems remain the same water authority. The reference cloud/sky light affects both through the common water material and reflection interfaces.

## Terrain vs ocean depth and rendering conflicts

A water plane may cover a terrain depth; don't let far cloud or atmospheric haze render *through* a submerged mountain. Ocean SSR/hybrid reflections must share scene-linear cloud sky radiance and adaptation state; avoid cloud shadows incorrectly affecting sky colour in ocean reflections. Eliminate duplicate water surfaces by making one renderer the water owner. Resolve glass reflection/refraction with consistent scene buffers; no overlay pass should multiply cloud transmittance twice.

**EXACT-ARCHIVE** KSP PQS_MANAGER for Jool sets deactivateDistance=175000, overrideKillSphere altitude=-75000. This is a KSP workaround, not a physical engine radius or an instruction to destroy Orbit terrain beyond 175km. For an Orbit gas giant, create an explicit optical lower termination/absorption depth owned by planet atmosphere/cloud medium; never invent a solid rock surface.

## Wetness / puddle accumulation (reference-derived settings)

**EXACT-ARCHIVE** `EVE_WET_SURFACES_CONFIG` defines one Kerbin profile, linked by rain cloud layers:
- minCoverageThreshold=0.15; maxCoverageThreshold=1.0; puddleTextureScale=1500; rippleScale=10.
- Scenery: puddleAccumulationSpeed=0.01, maxPuddleAccumulation=0.75, puddleDryingSpeed=0.000025, wetnessAccumulationSpeed=0.03, wetnessDryingSpeed=0.0025, wetSmoothness=0.6.
- Terrain: puddleAccumulationSpeed=0.00333, puddleDryingSpeed=0.000015, wetnessAccumulationSpeed=0.001, wetnessDryingSpeed=0.00025, wetDiffuse=0.4.
- Craft: wetnessAccumulationSpeed=0.01, wetnessDryingSpeed=0.0025.
- Puddles and splash/ripple textures are missing from the provided ZIP.

It is **unknown whether speed units are seconds, Unity frame scale or normalized curve inputs**; do not blindly declare these numbers as SI per-second rates. A proposed Orbit state model uses physically specified independent rates after calibration:

```cpp
struct WetSurfaceState {
    float film=0.0f;      // 0 dry .. 1 wet
    float puddle=0.0f;    // world-material-specific capacity
    double lastUpdatedSeconds=0.0;
    u64 authoritativeWeatherRevision=0;
};
void UpdateWetSurface(
    WetSurfaceState& s, double nowSeconds, float rainIntensity,
    const WetMaterialProfile& material)
{
    const double dt=std::clamp(nowSeconds-s.lastUpdatedSeconds,0.0,60.0);
    // For huge timewarp evaluate analytically or use weather playback
    // integration over elapsed interval; never iterate one step per second.
    const float wetSource=material.filmWetRate*
        std::clamp(rainIntensity,0.0f,1.0f);
    s.film=std::clamp(s.film+static_cast<float>(dt)*
        (wetSource-material.filmDryRate),0.0f,1.0f);
    const float puddleSource=material.puddleRate*rainIntensity*s.film;
    s.puddle=std::clamp(s.puddle+static_cast<float>(dt)*
        (puddleSource-material.puddleDrainRate),0.0f,material.capacity);
    s.lastUpdatedSeconds=nowSeconds;
}
```

This is **PROPOSED-PORT**, not copied V5 integration. Real terrain slope, permeability, puddle capacity, drainage, evaporation, wind/drying and owner material should affect the real Orbit model. Wetness must be world-anchored and able to persist/decay offscreen; never tie puddle UV to camera motion or cloud render resolution. Precipitation authority comes from shared weather/storm state, not a B/EVE shader alpha threshold alone.

### Material interaction

- Terrain wetness: alter roughness/diffuse and local puddle mask in the material service, not terrain heights unless actual water depth geometry is intended.
- Roads/railways/runways: use owner material policies (asphalt, gravel, steel, wood, concrete) and local drainage; cloud renderer emits rain intensity events, doesn't edit the road spline.
- Buildings/crafts: include roofs and window normals, material film and thickness, optional cockpit wetness and reflection response. A broken window changes precipitation ingress state via game physics, not a render-only event.
- Audio: rain hitting glass/metal and puddle impacts are events to sound domains; no GUI postprocess owns sound propagation.

## Wetness timescale and timewarp test

For long skip dt, integrate rain accumulation from climate time buckets or storm checkpoint spans, not an unbounded for-loop. On world load, restore wetness persistently or analytically reconstruct from recent weather. Cache invalidation separates weather change (precipitation) from material ownership/geometry change (wetness response). Scene snapshots include a wetness revision used by rendering/reflection resources.

## Acceptance gates

1. Cloud shadows aligned with terrain at different clipmap sizes, origin shifts and from orbit; arbitrary camera zoom must not slide them.
2. Sun behind a mountain yields no direct cloud/terrain solar term even when angular Sun is above mathematical horizon. Finite-disc softness tested.
3. Ships and buildings correctly block cloud/atmosphere volume through opaque depth without duplicating shadow factors.
4. Ocean above terrain: one water surface, coherent reflection and refraction, cloud shadows and lighting match sky radiance.
5. Wetness/puddles persist offscreen, timewarp is bounded, objects do not accumulate on camera-relative coordinates.
6. Rain/snow/dust event shapes and material results driven by weather, not by visual alpha or Culling LOD.
7. Reflection captures sample a consistent cloud LOD and pass exact version/exposure metadata.
