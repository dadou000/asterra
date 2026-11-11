+++
path = "/rendering/clouds/reference-eve-v5-fx"
title = "EVE V5 precipitation, lightning, particles, droplets and audio events"
kind = "concept"
status = "proposed"
owner_module = "OrbitCelestialClouds"
summary = "Physical event ownership and V5 authored submodules for lightning, droplets, rain/snow/dust, particle splashes, wetness, wind and sound."
keywords = ["lightning", "storm", "precipitation", "rain", "snow", "dust", "particles", "cockpit", "droplets", "audio"]
related = ["/rendering/clouds/reference-eve-v5", "/rendering/clouds/reference-eve-v5-terrain"]
depends_on = ["/rendering/clouds/reference-eve-v5-config"]
+++

# Component manifest (EXACT-ARCHIVE)

The V5 ZIP contains the following separate authored behavior libraries:
- EVE_LIGHTNING_CONFIG / OBJECT: **5** profiles; includes `lightIntensity`, `lightColor`, `lightRange`, `spawnAltitude`, `spawnChancePerSecond`, `minSpawnRange`, `soundMaxDistance`, `boltWidth`, `boltHeight`, `lightningSheetCount`, `boltTexture`, and `nearSounds / farSounds / Item`.
- EVE_PARTICLE_FIELD_CONFIG / OBJECT: **4** profiles (`rain-Kerbin`, `snow-Kerbin`, `dust-Duna`, `snow-Laythe`). Particle stretch, count, field size, fall speed, randomized drift, atlas size and splash section are authored values.
- EVE_DROPLETS_CONFIG / OBJECT: **2** profiles (`droplets-rain-Kerbin` and `droplets-cloud-Kerbin`), with distinct side/top droplet layers, relative wind streaks, refractive/translucent properties and drying speeds.
- EVE_WET_SURFACES_CONFIG: **1** profile `wet-surfaces-Kerbin` linked to cloud layers; detailed material policy is documented in terrain/water chapter.
- EVE_CLOUDS includes `lightning` links in 5 layers, `particleField` in 7, `droplets` in 5, `ambientSound` in 12, `wetSurfaces` in 2, 9 timeSettings and two fxOnly snow volumes.
- The supplied ZIP is missing the referenced lightning sprite sheets, precipitation textures, sounds and cockpit droplet noise. The proprietary Epidemic Sounds licensing explicitly prevents copying its audio into public Orbit assets.

## Example released numeric parameters

**EXACT-ARCHIVE** `lightning-Kerbin`: lightIntensity=2, lightColor=130,140,255,255, lightningSheetCount=4,3, boltHeight=2500, minSpawnRange=20000, soundMaxDistance=25000, spawnChancePerSecond=2. These are authoring sliders/units used by KSP; the emitted radiometric flash strength cannot be derived from lightIntensity alone.

`rain-Kerbin`: randomDirectionStrength=0.5, particleStretch=20, fieldParticleCount=200000, fieldSize=70, fallSpeed=12, particleSize=0.03, minCoverageThreshold=0.15, sheetCount=6x1, optional splashes atlas 4x4 with size=0.4x0.4. `snow-Kerbin`: count=400000, fieldSize=100, fallSpeed=0.75, size=0.05, sheetCount=12x12. `dust-Duna`: count=300000, fieldSize=50, tangentialSpeed=50, randomDirectionStrength=10, size=0.01. **Do not** assume fieldParticleCount is a global planet-wide continuous particle count; the package does not expose the runtime culling/ownership algorithm.

`droplets-cloud-Kerbin` sets translucent=0.97, speedIncreaseFactor=0.15, dryingSpeed=0.02, fadeOutEndSpeed=250, noise/triplanar properties; both droplets profiles have fast and slow side streak layers with different fallSpeed/streakRatio and one top droplet layer.

# Event authority: do not simulate the same bolt twice

Orbit's storm solver/electrification is the physical source of lightning, precipitation and wind. Existing authoritative GPU storm data and planned sound domains must produce deterministic, timestamped events. EVE reference configuration is a **rendering/authoring profile** for those events; the renderer must not independently roll random storm spawns that diverge from physics and audio.

**PROPOSED-PORT**:

```cpp
struct WeatherEventId {
    u64 stormHigh, stormLow;
    u64 sequence;
};
enum class WeatherEventKind : u8 {
    LightningLeader, LightningReturnStroke, LightningAfterglow,
    RainField, SnowField, DustField, CloudWake, GroundSplash,
    WindowDroplets, StormWind
};
struct WeatherEvent {
    WeatherEventId id;
    WeatherEventKind kind;
    universe::BodyId body;
    math::Double3 originBodyMeters;
    math::Double3 directionBody;
    double absoluteTimeSeconds;
    double durationSeconds;
    float physicalIntensity;
    float affectedRadiusMeters;
    u64 simulationRevision;
    u32 materialOrProfile;
};
class WeatherEventStream {
public:
    std::span<const WeatherEvent> Query(
        universe::BodyId body, double fromTime, double toTime) const;
    u64 Revision(universe::BodyId body) const noexcept;
};
```

One event can drive several independent consumers: Lightning geometry/flash lighting, surface wetness/ripple splashes, ambient audio and sound propagation, camera/vehicle droplets, TAA history invalidation. GPU particle visualisation is not allowed to own lightning timing or reset physical storms.

## Lightning optical and acoustic lifecycle

The physical event's leader branches, return stroke, multiple-stroke sequence, side channels, and emissivity must be represented as separate temporal states. EVE's bolt atlas is a rendering reference but not physical propagation authority.

**PROPOSED-PORT** lifecycle:
- Authoritative storm model schedules electrical discharge and candidate path in world/body coordinates. Cache deterministic event ID, path and physical timestamp.
- Leader/streamer phase may be relatively dim; return-stroke is extremely brief and bright. The final emissive path can affect clouds, rain sheets and glass with a local volumetric light budget; invalidate affected light-cache cells promptly instead of resetting the entire planet.
- Emit one radiometric transient light using Orbit `TransientFlashRegistry`/`LocalLightRegistry`; do not add a new global Sun. Distribute radiance along visible arc or approximate with a bounded set of emitters.
- Let the Sun source and finite-disc visibility stay independent of lightning; no eclipse state gets modified by a flash.
- Emit thunder audio from the *same event ID*, but the acoustic solver determines arrival/diffraction/reflections. A light flash is visible nearly immediately; thunder arrives after sound propagation through the actual domains/materials. Do not schedule all listeners by a fixed KSP near/far sound cutoff.

```cpp
void PublishLightningToConsumers(const WeatherEvent& e,
    FlashRegistry& flashes, CloudLightCache& cloudLight,
    AcousticEventQueue& sound, RenderHistoryTracker& history)
{
    if(e.kind!=WeatherEventKind::LightningReturnStroke) return;
    flashes.Publish({
        .worldEvent=e.id.sequence,
        .body=e.body,
        .positionMeters=e.originBodyMeters,
        .physicalIntensity=e.physicalIntensity,
        .timeSeconds=e.absoluteTimeSeconds
    });
    cloudLight.InvalidateVolume(
        e.originBodyMeters,e.affectedRadiusMeters,e.simulationRevision);
    history.MarkTransient(e.body,e.originBodyMeters,e.affectedRadiusMeters);
    sound.PublishLightning(e.id,e.body,e.originBodyMeters,
        e.absoluteTimeSeconds);
}
```

These named adapter methods are **new Orbit interfaces to implement**, not existing symbols. Near/far thunder samples can be used only if the user owns appropriate licenses and through sound domains. The effective waveform is defined by material transmission, openings, rain and atmospheric attenuation, not just distance.

## Rain, snow and dust GPU particle fields

Use GPU-spawned field tiles around active listeners/cameras with deterministic spatial hashing; one simulation description per weather region, per-view rendering only. Chunk/particle count adapts by LOD; never spawn 200k across every planetary tile every frame. A particle's world position is body-fixed plus physical fall/advection, while GPU positions are relative to a stable local anchor.

```cpp
struct PrecipitationFieldDescriptor {
    universe::BodyId body;
    math::Double3 centreBodyMeters;
    math::Double3 windMetersPerSecond;
    math::Float3 tintLinear;
    float areaRadiusMeters, heightMeters;
    float intensity, fallSpeedMetersPerSecond;
    float particleSizeMeters, spriteStretch;
    u32 atlasColumns, atlasRows;
    u32 maxVisibleParticles;
    u64 weatherRevision, seed;
};
struct PrecipitationRenderInputs {
    const rhi::Texture* sceneDepth;
    const rhi::Buffer* terrainCollisionGrid;
    const rhi::Buffer* bodyCloudVisibility;
    const rhi::Buffer* localLighting;
    const rhi::Texture* previousFrameMotion;
};
```

Particles:
- Rain gets anisotropic streaks, splash impacts, droplet onset and optional puddle/ripple events.
- Snow uses slower fall, broader drift and sprite LOD; do not treat snow optical extinction as rain.
- Dust storms use strong horizontal advection, lower albedo, occlusion and fine airborne scattering with ground dust entrainment.
- Foam/splash/rain texture imports require atlas swizzle, UV pivot, sheet columns/rows, particle age/velocity, soft depth intersections and world-space collision.
- Light them from the same Sun/sky/flash sources as clouds and surfaces, with cloud receiver transmittance appropriate to particle point. V5 changelog says particles use ambient reflection probe and light-volume shadow; Orbit replaces those with its physically unified light query.

### Spatial budget and timewarp

Simulate/retain aggregate precipitation flux independent of visible particles. GPU particles can respawn deterministically on camera relocation/timewarp; the total mass/rain rate for wetness remains authoritative. On 1000x timewarp never process each missed particle in a queue. Integrate population statistics or switch to replay. Physically persistent wetness must not vanish when particle rendering is disabled.

## Droplets on cockpit glass, windows and vehicle surfaces

V5 cockpit droplets are configured with high/low-speed streak response, acceleration, triplanar noise, side/top lanes, refraction/transparency and speed-dependent drying. This is **material-interface** behavior, not a fullscreen camera texture glued to the monitor. Existing Orbit glass may be expensive; integrate droplets through the surface/material path using a low-cost mask/normal-flow atlas, keeping SSR/offscreen refraction policy coherent.

Proposed CPU state:
```cpp
struct GlassWetFilm {
    u64 surfaceId;
    float depositedWater;
    float streakCoverage;
    math::Float2 bodyTangentRunoff;
    math::Float2 relativeAirflowMetersPerSecond;
    double lastUpdateSeconds;
    u64 weatherRevision, materialRevision;
};
void AdvanceGlassFilm(GlassWetFilm& film,
    const WeatherFlux& rain, const VehicleSurfaceState& surface,
    double physicalDt);
```

Physical inputs: vehicle relative wind, gravity projected onto glass surface, acceleration, droplet adhesion, hydrophobicity, glass tilt, roof shelter, cracks/openings and local sound. Fast runoff can drain/smear wet patches; snow collects differently. Underwater droplets are invalid unless an actual air/water interface is present. Temporal upscaler must either use correct surface motion vectors or bypass history for rapidly moving thin droplets.

## Ambient sound and sound domains

The V5 config names ambientSound linked to layer regions. Orbit's acoustic domain graph is authoritative: source intensity and spectrum depend on real rain/storm/wind, while transmission into cabins/towers depends on concrete/glass/vents/open windows/material vibration. Do not hardcode EVE's near/far sample position into game sound physics. A thunder flash, wind gust and precipitation field share their weather event ID and absolute time. Offscreen visuals may be culled while sound remains active.

Sound asset license: no redistributing proprietary Epidemic Sounds clips. Use existing legitimately licensed Orbit sound packs or generative physically based signals; preserve only the semantic event bindings from the config.

## Tests

1. One lightning event ID causes one scene flash, one sound source and one history invalidation in the correct physical order; replay/timewarp does not duplicate it.
2. Rain near ground deposits on appropriate wet surfaces regardless of viewport or visibility; toggling particles off changes no physical accumulation.
3. Sheet atlas UVs/sample depths correct and particles never draw through opaque terrain/ship.
4. Fast cockpit motion and transparent glass receive separate motion vectors; TAA cannot smear droplet trails over distant clouds.
5. Lightning flash appears in cloud medium and sky in a bounded spatial region, including negative/extreme solar angle; does not alter global Sun cache stamp.
6. Weather at 1000x timewarp does not grow per-frame particle/event catch-up work without bound; user can restore replay to solver state.
7. Audio through nested sound domains obeys vents/glass/structural transmission without relying on render pixel coverage.
