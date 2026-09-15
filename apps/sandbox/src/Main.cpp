#include <orbit/camera/FreeCamera.hpp>
#include <orbit/core/BuildInfo.hpp>
#include <orbit/core/Log.hpp>
#include <orbit/debug_render/VersionOverlayRenderer.hpp>
#include <orbit/dev_server/DevServer.hpp>
#include <orbit/fields/FieldRegistry.hpp>
#include <orbit/jobs/JobSystem.hpp>
#include <orbit/map_render/PlanetMapRenderer.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/platform/CrashHandler.hpp>
#include <orbit/platform/Window.hpp>
#include <orbit/rhi/vulkan/VulkanBackend.hpp>
#include <orbit/runtime/RuntimeSession.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <orbit/surface/SurfaceRegistry.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain_cache/CachedTerrainSource.hpp>
#include <orbit/terrain_gpu/GpuElevationQuery.hpp>
#include <orbit/terrain_gpu/GpuFieldGenerator.hpp>
#include <orbit/terrain_gpu/GpuRegionDelta.hpp>
#include <orbit/terrain_region/DerivedRegionTerrainSource.hpp>
#include <orbit/terrain_region/DerivedTerrainRegionCache.hpp>
#include <orbit/terrain_region/DerivedTerrainRegionStreamer.hpp>
#include <orbit/terrain_render/TerrainPreviewRenderer.hpp>
#include <orbit/terrain_render/UniformPlanetRenderer.hpp>
#include <orbit/terrain_stream/TerrainSampleStreamer.hpp>
#include <orbit/universe/BodyRegistry.hpp>
#include <orbit/water_render/RiverWaterRenderer.hpp>
#include <orbit/world/Planet.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <format>
#include <memory>
#include <stdexcept>
#include <vector>

namespace
{
[[nodiscard]] const char* MapLayerName(
    const orbit::map_render::MapLayer layer) noexcept
{
    switch (layer)
    {
        case orbit::map_render::MapLayer::Elevation:
            return "ELEVATION";
        case orbit::map_render::MapLayer::Tectonics:
            return "TECTONICS";
        case orbit::map_render::MapLayer::Biomes:
            return "BIOMES";
        case orbit::map_render::MapLayer::Temperature:
            return "TEMPERATURE";
        case orbit::map_render::MapLayer::Precipitation:
            return "PRECIPITATION";
    }
    return "UNKNOWN";
}
} // namespace

int main()
{
    try
    {
        orbit::log::Info(
            std::format(
                "Orbit M0 boot | {}",
                orbit::build::DisplayVersion));

        orbit::runtime::RuntimeSession runtime({
            .applicationName = "OrbitSandbox",
            .windowTitle = "Orbit - Asterra Engine",
            .width = 1600,
            .height = 900,
            .swapchainBufferCount = 3,
            .allowTearing = true,
            .relativeMouseMode = true
        });

        auto* window = &runtime.Window();
        auto* device = &runtime.Device();
        auto* graphicsQueue = &runtime.GraphicsQueue();
        auto* swapchain = &runtime.Swapchain();

        orbit::log::Info(
            "Camera controls: WASD move, mouse look, Q/E down/up, Shift boost, Esc quit.");

        const auto& capabilities = device->Capabilities();

        orbit::log::Info(std::format(
            "GPU: {} | Vulkan {}.{} | RT: {} | Mesh shaders: {} | VRS: {} | Tearing: {}",
            device->AdapterName(),
            capabilities.shaderModelMajor,
            capabilities.shaderModelMinor,
            capabilities.rayTracing,
            capabilities.meshShaders,
            capabilities.variableRateShading,
            capabilities.presentTearing
        ));

        auto depthTarget =
            device->CreateTexture({
                .width = swapchain->Width(),
                .height = swapchain->Height(),
                .format =
                    orbit::rhi::TextureFormat::D32_Float,
                .initialState =
                    orbit::rhi::ResourceState::DepthWrite
            });

        // Per-frame-in-flight GPU timing: 4 timestamps per swapchain
        // slot (scene begin/end, overlay begin/end), so a slot's
        // previous timestamps are only ever read back after the same
        // frame-fence wait that already guards reusing that slot's
        // allocator -- no separate synchronization needed.
        constexpr orbit::u32 kTimestampsPerFrame = 4;

        auto gpuTimestamps =
            device->CreateTimestampQueryPool(
                swapchain->BufferCount() *
                kTimestampsPerFrame);

        const orbit::f64 timestampPeriodNs =
            device->TimestampPeriodNanoseconds();

        orbit::f64 lastGpuSceneMs = 0.0;
        orbit::f64 lastGpuOverlayMs = 0.0;

        // Created here (rather than just before the render loop) so
        // GpuElevationQuery -- constructed below, before
        // applyObserverPosition captures it -- has a real frame fence
        // to check GPU-dispatch completion against. The render loop
        // further down uses these same objects exactly as it always
        // has; nothing about their own lifecycle changes, only when
        // they come into existence.
        std::vector<
            std::unique_ptr<
                orbit::rhi::CommandAllocator>>
            frameAllocators;

        frameAllocators.reserve(
            swapchain->BufferCount());

        for (orbit::u32 index = 0;
             index < swapchain->BufferCount();
             ++index)
        {
            frameAllocators.push_back(
                device->CreateCommandAllocator(
                    orbit::rhi::QueueType::Graphics));
        }

        auto commandList =
            device->CreateCommandList(
                *frameAllocators.front());

        auto frameFence =
            device->CreateFence(0);

        std::vector<orbit::u64> frameFenceValues(
            swapchain->BufferCount(),
            0);

        orbit::u64 nextFenceValue = 1;

        orbit::frames::FrameGraph celestialFrames;
        orbit::universe::BodyRegistry celestialBodies(
            celestialFrames);

        const orbit::universe::SystemId helionSystem =
            celestialBodies.CreateSystem("Helion");

        const orbit::universe::BodyId asterraBodyId =
            celestialBodies.CreateBody({
                .system = helionSystem,
                .name = "Asterra",
                .shape =
                    orbit::universe::SphereShape{
                        .radiusMeters =
                            6'000'000.0
                    },
                .mass =
                    orbit::universe::MassProperties{
                        .massKilograms =
                            5.0e24
                    },
                .transformModel =
                    orbit::universe::
                        FixedBodyTransform{}
            });

        // A second body is registered even though this sandbox currently
        // renders Asterra only. This makes the composition root exercise
        // the multi-body universe path instead of retaining a hidden
        // one-planet assumption.
        static_cast<void>(
            celestialBodies.CreateBody({
                .system = helionSystem,
                .name = "Luma",
                .shape =
                    orbit::universe::SphereShape{
                        .radiusMeters =
                            1'500'000.0
                    },
                .transformModel =
                    orbit::universe::
                        FixedBodyTransform{
                            .parentFromBody = {
                                .translation = {
                                    400'000'000.0,
                                    0.0,
                                    0.0
                                }
                            }
                        }
            }));

        orbit::surface::SurfaceRegistry bodySurfaces(
            celestialBodies);

        const auto planetDefinition =
            bodySurfaces.
                SphericalPlanetDefinition(
                    asterraBodyId);

        if (!planetDefinition.has_value())
        {
            throw std::runtime_error(
                "Asterra requires a spherical reference surface.");
        }

        const orbit::world::PlanetDefinition planet =
            *planetDefinition;

        const orbit::math::Double3 observerDirection =
            orbit::math::Normalize(
                orbit::math::Double3{
                    0.365111510558,
                    0.187057495117,
                    -0.911977564625
                });

        const orbit::world::SurfaceFrame
            initialSurfaceFrame =
                orbit::world::MakeSurfaceFrame(
                    observerDirection);

        const orbit::terrain::AnalyticTerrainDesc
            terrainDescription{
                .seed = 0xA57E22AULL
            };

        const auto authoritativeTerrain =
            std::make_shared<
                orbit::terrain::AnalyticTerrainSource>(
                    planet,
                    terrainDescription);

        bodySurfaces.AttachTerrain(
            asterraBodyId,
            authoritativeTerrain);

        orbit::fields::FieldRegistry fieldRegistry;

        static_cast<void>(
            fieldRegistry.Register({
                .descriptor = {
                    .ownerBody = asterraBodyId,
                    .name = "Elevation",
                    .valueKind =
                        orbit::fields::
                            FieldValueKind::Scalar,
                    .domain =
                        orbit::fields::
                            FieldDomain::Surface,
                    .unit = "m",
                    .residency =
                        orbit::fields::
                            FieldResidency::Cpu,
                    .resolution = {
                        .mode =
                            orbit::fields::
                                FieldResolutionMode::
                                    AdaptiveLod
                    }
                },
                .cpuEvaluator =
                    [authoritativeTerrain](
                        const orbit::fields::
                            FieldLocation& location)
                        -> std::optional<
                            orbit::fields::
                                FieldValue>
                    {
                        const auto* surface =
                            std::get_if<
                                orbit::fields::
                                    SurfaceFieldLocation>(
                                        &location);

                        if (surface == nullptr)
                        {
                            return std::nullopt;
                        }

                        return orbit::fields::
                            FieldValue(
                                authoritativeTerrain->
                                    Sample({
                                        .unitDirection =
                                            surface->
                                                unitDirection,
                                        .footprintMeters =
                                            surface->
                                                footprintMeters
                                    }).elevationMeters);
                    },
                .revision =
                    [authoritativeTerrain]
                    {
                        return
                            authoritativeTerrain->
                                Revision();
                    }
            }));

        static_cast<void>(
            fieldRegistry.Register({
                .descriptor = {
                    .ownerBody = asterraBodyId,
                    .name = "Temperature",
                    .valueKind =
                        orbit::fields::
                            FieldValueKind::Scalar,
                    .domain =
                        orbit::fields::
                            FieldDomain::Surface,
                    .unit = "degC",
                    .residency =
                        orbit::fields::
                            FieldResidency::Cpu,
                    .resolution = {
                        .mode =
                            orbit::fields::
                                FieldResolutionMode::
                                    AdaptiveLod
                    }
                },
                .cpuEvaluator =
                    [authoritativeTerrain](
                        const orbit::fields::
                            FieldLocation& location)
                        -> std::optional<
                            orbit::fields::
                                FieldValue>
                    {
                        const auto* surface =
                            std::get_if<
                                orbit::fields::
                                    SurfaceFieldLocation>(
                                        &location);

                        if (surface == nullptr)
                        {
                            return std::nullopt;
                        }

                        return orbit::fields::
                            FieldValue(
                                static_cast<orbit::f64>(
                                    authoritativeTerrain->
                                        Sample({
                                            .unitDirection =
                                                surface->
                                                    unitDirection,
                                            .footprintMeters =
                                                surface->
                                                    footprintMeters
                                        }).climate.
                                            temperatureC));
                    },
                .revision =
                    [authoritativeTerrain]
                    {
                        return
                            authoritativeTerrain->
                                Revision();
                    }
            }));

        const orbit::shader::dxc::DxcShaderCompiler
            shaderCompiler;

        // The clipmap's near-field terrain is generated on the GPU
        // (see engine/terrain_gpu) directly from the raw analytic
        // recipe, then composited against the fine region cache's own
        // GPU-computed hydrology (erosion/lake-fill -- see
        // GpuHydrologyRegion and GpuRegionDelta) right after
        // generation, below. The region caches themselves also run
        // hydrology/erosion/river/lake generation on the GPU (see
        // DerivedTerrainRegionCacheConfig::gpuHydrology) -- so the 2D
        // map, river/lake water rendering, and the clipmap all show
        // the same GPU-computed hydrology.
        orbit::terrain_gpu::GpuFieldGenerator
            gpuFieldGenerator(
                *device,
                shaderCompiler,
                planet,
                *authoritativeTerrain);

        // Shared by both region caches below -- one GPU hydrology
        // pipeline instance, sized for the larger of their two
        // (currently identical) region resolutions.
        orbit::terrain_gpu::GpuHydrologyRegion
            gpuHydrologyRegion(
                *device,
                shaderCompiler,
                gpuFieldGenerator,
                129);

        orbit::jobs::JobSystem jobSystem;

        auto regionCache =
            std::make_shared<
                orbit::terrain_region::
                    DerivedTerrainRegionCache>(
                        planet,
                        authoritativeTerrain,
                        jobSystem,
                        orbit::terrain_region::
                            DerivedTerrainRegionCacheConfig{
                                .tileLevel = 5,
                                .maxEntries = 12,
                                .region = {
                                    .generatorVersion = 2,
                                    .overlapScale = 1.35
                                },
                                .gpuDevice = device,
                                .gpuHydrology = &gpuHydrologyRegion,
                                .gpuFence = frameFence.get()
                            });

        // A second, finer-tiled region cache covering only the
        // immediate neighborhood of the observer. Hydrology/erosion/
        // river generation uses the same fixed grid resolution
        // (DerivedTerrainRegionConfig::hydrology) regardless of tile
        // level, so a physically smaller tile is proportionally
        // finer -- level 9 tiles are ~1/16th the width of the level
        // 5 coarse tiles, giving ~16x finer sample spacing (roughly
        // 3km -> ~200m) right around the camera, where the coarse
        // grid's sparse river/carving nodes visibly let raw terrain
        // poke back up through the river between them. Everywhere
        // outside this near-field neighborhood, rendering and
        // elevation still fall back to the coarse cache unchanged.
        auto fineRegionCache =
            std::make_shared<
                orbit::terrain_region::
                    DerivedTerrainRegionCache>(
                        planet,
                        authoritativeTerrain,
                        jobSystem,
                        orbit::terrain_region::
                            DerivedTerrainRegionCacheConfig{
                                .tileLevel = 9,
                                .maxEntries = 16,
                                .region = {
                                    .generatorVersion = 2,
                                    .overlapScale = 1.35
                                },
                                .gpuDevice = device,
                                .gpuHydrology = &gpuHydrologyRegion,
                                .gpuFence = frameFence.get()
                            });

        orbit::terrain_region::
            DerivedTerrainRegionStreamer
                fineRegionStreamer(
                    planet,
                    *fineRegionCache,
                    {
                        .neighborhoodRadius = 1,
                        .forwardPrefetchDistanceTiles =
                            1.0
                    });

        const auto streamedTerrain =
            std::make_shared<
                orbit::terrain_region::
                    DerivedRegionTerrainSource>(
                        planet,
                        authoritativeTerrain,
                        regionCache,
                        fineRegionCache);

        orbit::terrain_region::
            DerivedTerrainRegionStreamer
                regionStreamer(
                    planet,
                    *regionCache,
                    {
                        .neighborhoodRadius = 1,
                        .forwardPrefetchDistanceTiles =
                            1.0
                    });

        orbit::terrain_cache::CachedTerrainSource
            terrain(
                planet,
                streamedTerrain,
                jobSystem,
                {
                    .pageResolution = 33,
                    .requestMissThreshold = 24,
                    .minimumTileLevel = 0,
                    .maximumTileLevel = 24,
                    .cache = {
                        .budgetBytes =
                            512ULL * 1024ULL * 1024ULL,
                        .softEntryLimit =
                            16384
                    }
                });

        orbit::log::Info(
            std::format(
                "Terrain workers: {}",
                jobSystem.WorkerCount()));

        // Start above a surveyed mountain range, with clearance derived from
        // the authoritative terrain rather than an assumed sea-level height.
        const orbit::f64 initialGroundElevationMeters =
            authoritativeTerrain->Sample({observerDirection, 1.0}).elevationMeters;
        const orbit::f64 initialAltitudeMeters =
            std::max(8'000.0, initialGroundElevationMeters + 2'500.0);

        orbit::world::WorldPosition observer{
            .meters =
                observerDirection *
                (planet.radiusMeters + initialAltitudeMeters)
        };

        orbit::world::SurfaceFrame
            observerTravelFrame =
                initialSurfaceFrame;

        orbit::math::Double3
            lastSurfaceTravelDirection{};

        // Height above the ground sampled after the previous
        // frame's move; feeds this frame's altitude-based speed
        // curve (one frame of lag, imperceptible).
        orbit::f64
            lastAltitudeAboveGroundMeters =
                initialAltitudeMeters - initialGroundElevationMeters;

        // A screenshot must be captured from a *later* frame than
        // the one that raised the window, since raising it cannot
        // itself force a fresh present through an occluded
        // swapchain -- see Window::RaiseToTop.
        struct PendingScreenshot
        {
            bool active{false};
            std::string path;
            int framesRemaining{0};
        };

        PendingScreenshot pendingScreenshot;

        // A smooth multi-second flight to a target point, driven by
        // the dev server (SLEW) rather than the keyboard, so a test
        // harness can watch the clipmap actually stream and morph
        // while moving instead of only ever seeing instantaneous
        // teleport jumps.
        struct SlewState
        {
            bool active{false};
            orbit::math::Double3 fromDirection{};
            orbit::math::Double3 toDirection{};
            orbit::f64 fromAltitudeMeters{0.0};
            orbit::f64 toAltitudeMeters{0.0};
            orbit::f64 elapsedSeconds{0.0};
            orbit::f64 durationSeconds{1.0};
        };

        SlewState slewState;

        orbit::debug_render::VersionOverlayRenderer
            versionOverlay(
                *device,
                shaderCompiler,
                orbit::build::DisplayVersion);

        // F3 debug HUD: a stack of small text panels pinned to the
        // top-left corner, updated live and toggled at runtime.
        constexpr orbit::u32
            kDebugOverlayLineCount = 12;

        std::vector<
            std::unique_ptr<
                orbit::debug_render::
                    VersionOverlayRenderer>>
            debugOverlayLines;

        debugOverlayLines.reserve(
            kDebugOverlayLineCount);

        for (orbit::u32 lineIndex = 0;
             lineIndex <
                kDebugOverlayLineCount;
             ++lineIndex)
        {
            orbit::debug_render::
                VersionOverlayConfig
                    lineConfig{};

            lineConfig.anchor =
                orbit::debug_render::
                    OverlayAnchor::TopLeft;

            constexpr orbit::u32
                lineHeight = 28;

            lineConfig.extraTopMarginPixels =
                lineIndex * lineHeight;

            debugOverlayLines.push_back(
                std::make_unique<
                    orbit::debug_render::
                        VersionOverlayRenderer>(
                    *device,
                    shaderCompiler,
                    "ORBIT",
                    lineConfig));
        }

        bool debugOverlayVisible = false;
        bool f3PressedLastFrame = false;
        std::string lastStatsLine;

        // F2 debug visuals menu: LOD lattice coloring, frozen
        // generation, and a side cutaway through the clipmap -- see
        // TerrainPreviewRenderer::SetDebugVisuals/SetGenerationFrozen.
        // Drawn as its own small stack of overlay lines, positioned
        // right below the F3 HUD's own 12 lines so the two never
        // overlap even when both are visible at once.
        constexpr orbit::u32
            kDebugVisualsLineCount = 4;

        std::vector<
            std::unique_ptr<
                orbit::debug_render::
                    VersionOverlayRenderer>>
            debugVisualsLines;

        debugVisualsLines.reserve(
            kDebugVisualsLineCount);

        for (orbit::u32 lineIndex = 0;
             lineIndex <
                kDebugVisualsLineCount;
             ++lineIndex)
        {
            orbit::debug_render::
                VersionOverlayConfig
                    lineConfig{};

            lineConfig.anchor =
                orbit::debug_render::
                    OverlayAnchor::TopLeft;

            constexpr orbit::u32
                lineHeight = 28;

            lineConfig.extraTopMarginPixels =
                (kDebugOverlayLineCount +
                 lineIndex) *
                lineHeight;

            debugVisualsLines.push_back(
                std::make_unique<
                    orbit::debug_render::
                        VersionOverlayRenderer>(
                    *device,
                    shaderCompiler,
                    "ORBIT",
                    lineConfig));
        }

        bool debugVisualsMenuVisible = false;
        bool f2PressedLastFrame = false;
        bool debugLodColorEnabled = false;
        bool debugSideCutEnabled = false;
        bool debugGenerationFrozen = false;
        bool lPressedLastFrame = false;
        bool gPressedLastFrame = false;
        bool cPressedLastFrame = false;

        bool wholePlanetLodForced = false;
        orbit::i32 forcedPlanetLod = 0;
        bool f4PressedLastFrame = false;
        bool planetLodDecPressedLastFrame = false;
        bool planetLodIncPressedLastFrame = false;

        orbit::terrain_render::TerrainPreviewConfig
            terrainPreviewConfig{};

        terrainPreviewConfig.framesInFlight =
            swapchain->BufferCount();

        // Async GPU->CPU ground-elevation readback for camera
        // ground-clamp/teleport/slew -- see engine/terrain_gpu's
        // GpuElevationQuery and the GPU terrain generation plan's
        // Milestones 3 and 4. Runs the full local hydrology stack
        // (depression fill + erosion) per query, not just the raw
        // field, so collision rests on eroded ground / lake surfaces
        // consistently with the rest of the GPU terrain pipeline.
        // Shares the render loop's own frameFence so it can tell when
        // a queued dispatch has actually retired.
        orbit::terrain_gpu::GpuElevationQuery
            gpuElevationQuery(
                *device,
                shaderCompiler,
                gpuFieldGenerator,
                authoritativeTerrain->GlobalFields(),
                *frameFence);

        // Composites the fine region cache's GPU-computed hydrology
        // (erosion/lake-fill) into the clipmap's own GPU-generated
        // samples, in place, right after generation -- see
        // TerrainPreviewRenderer's constructor comment and
        // engine/terrain_gpu's GpuRegionDelta. The fine cache (not the
        // coarse one) is used since it's the higher-detail match for
        // what the near-field clipmap actually renders.
        orbit::terrain_gpu::GpuRegionDelta
            gpuRegionDelta(
                *device,
                shaderCompiler);

        orbit::terrain_render::TerrainPreviewRenderer
            terrainPreview(
                *device,
                shaderCompiler,
                planet,
                gpuFieldGenerator,
                observer,
                terrainPreviewConfig,
                &gpuRegionDelta,
                fineRegionCache.get());

        // F4 force-LOD: renders the whole closed planet as one fixed-
        // resolution mesh instead of the altitude-adaptive near-field
        // clipmap, so a specific LOD can be inspected across the
        // entire sphere on demand rather than only wherever the
        // observer happens to be.
        orbit::terrain_render::UniformPlanetRenderer
            uniformPlanet(
                *device,
                shaderCompiler,
                planet,
                authoritativeTerrain,
                terrainPreviewConfig);

        // M key: full-screen planet map with switchable layers
        // (elevation/tectonics/biomes/temperature/precipitation),
        // click-to-teleport. Built once against the same authoritative
        // terrain source used for ground collision.
        orbit::map_render::PlanetMapRenderer
            planetMap(
                *device,
                shaderCompiler,
                *graphicsQueue,
                authoritativeTerrain);

        bool mapVisible = false;
        bool mPressedLastFrame = false;
        bool leftClickPressedLastFrame = false;

        orbit::water_render::RiverWaterRenderer
            riverWater(
                *device,
                shaderCompiler,
                planet,
                regionCache,
                fineRegionCache,
                observer,
                {
                    .framesInFlight =
                        swapchain->BufferCount(),
                    .maximumSegments = 16'384,
                    .maximumLakeCells = 0,
                    .verticalFovRadians =
                        terrainPreviewConfig.
                            verticalFovRadians,
                    .nearPlaneMeters =
                        terrainPreviewConfig.
                            nearPlaneMeters,
                    .farPlaneMeters =
                        terrainPreviewConfig.
                            farPlaneMeters,
                    .maximumDrawDistanceMeters =
                        180'000.0,
                    .surfaceOffsetMeters =
                        0.08
                });

        orbit::log::Info(std::format(
            "Terrain preview: {} vertices, {} indices",
            terrainPreview.VertexCount(),
            terrainPreview.IndexCount()
        ));

        // Places the observer along `direction` at `desiredAltitudeMeters`
        // above the base sphere, clamped so it never sinks through the
        // actual sampled terrain, and propagates the new position to
        // every renderer that tracks it. Shared by keyboard movement,
        // TELEPORT, and SLEW so the ground-collision rule can't drift
        // out of sync between them.
        //
        // Ground elevation comes from GpuElevationQuery::LastKnownGood
        // rather than a direct, synchronous CPU
        // AnalyticTerrainSource::Sample() call -- it answers instantly
        // every time (a cached nearby GPU readback, or a cheap coarse
        // analytic estimate before the first one lands), while
        // Request() keeps a fresh, ground-truth GPU sample for this
        // exact direction flowing in behind it for next call. A few
        // frames of latency/approximation here is imperceptible for a
        // clearance check.
        const auto applyObserverPosition =
            [&observer,
             &planet,
             &gpuElevationQuery,
             &lastAltitudeAboveGroundMeters,
             &terrainPreview,
             &riverWater](
                const orbit::math::Double3&
                    direction,
                const orbit::f64
                    desiredAltitudeMeters)
        {
            static_cast<void>(
                gpuElevationQuery.Request(direction));

            const orbit::f64 groundElevationMeters =
                gpuElevationQuery.LastKnownGood(
                    direction);

            constexpr orbit::f64
                minClearanceMeters = 2.0;

            const orbit::f64
                minAltitudeMeters =
                    groundElevationMeters +
                    minClearanceMeters;

            const orbit::f64 altitude =
                std::clamp(
                    desiredAltitudeMeters,
                    minAltitudeMeters,
                    2'000'000.0);

            observer.meters =
                direction *
                (planet.radiusMeters +
                 altitude);

            lastAltitudeAboveGroundMeters =
                altitude -
                groundElevationMeters;

            terrainPreview.UpdateObserver(
                observer);


            riverWater.UpdateObserver(
                observer);
        };

        // Loopback-only test/automation hook: lets an external
        // harness (e.g. an MCP bridge) inspect and drive a running
        // Orbit process -- read stats, grab a screenshot, teleport
        // the observer, request a clean shutdown.
        bool remoteQuitRequested = false;

        orbit::dev_server::DevServer
            devServer({.port = 4319});

        devServer.RegisterCommand(
            "CACHE_LEVELS",
            [&terrain](const auto&)
            {
                const auto byLevel =
                    terrain.
                        PageCacheEntriesByLevel();

                std::string line;

                for (std::size_t level = 0;
                     level <
                        byLevel.size();
                     ++level)
                {
                    const auto& [count, bytes] =
                        byLevel[level];

                    if (count == 0)
                    {
                        continue;
                    }

                    line +=
                        std::format(
                            "L{}={}({}KiB) ",
                            level,
                            count,
                            bytes /
                                1024);
                }

                return line.empty()
                    ? std::string(
                        "ERR no cache entries")
                    : line;
            });

        devServer.RegisterCommand(
            "PING",
            [](const auto&)
            {
                return std::string("PONG");
            });

        devServer.RegisterCommand(
            "STATS",
            [&lastStatsLine](const auto&)
            {
                return lastStatsLine.empty()
                    ? std::string(
                        "ERR no stats yet")
                    : lastStatsLine;
            });

        devServer.RegisterCommand(
            "RENDERDOC_CAPTURE",
            [&device](const auto&)
            {
                if (!orbit::rhi::vulkan::IsRenderDocAvailable(*device))
                {
                    return std::string(
                        "ERR RenderDoc unavailable -- relaunch with "
                        "ORBIT_RENDERDOC=1 set (and renderdoc.dll "
                        "installed or ORBIT_RENDERDOC_DLL pointing at "
                        "it)");
                }

                orbit::rhi::vulkan::TriggerRenderDocCapture(*device);

                return std::string(
                    "OK capturing next frame");
            });

        devServer.RegisterCommand(
            "RENDERDOC_STATUS",
            [&device](const auto&)
            {
                if (!orbit::rhi::vulkan::IsRenderDocAvailable(*device))
                {
                    return std::string("unavailable");
                }

                const std::string lastCapture =
                    orbit::rhi::vulkan::LastRenderDocCapturePath(
                        *device);

                return std::format(
                    "available capturing={} last={}",
                    orbit::rhi::vulkan::IsRenderDocCapturing(*device),
                    lastCapture.empty() ? "(none)" : lastCapture);
            });

        devServer.RegisterCommand(
            "SCREENSHOT",
            [&window,
             &pendingScreenshot](
                const std::vector<
                    std::string>&
                    arguments)
            {
                if (arguments.empty())
                {
                    return std::string(
                        "ERR usage: SCREENSHOT <path.bmp>");
                }

                window->RaiseToTop();

                // Give the render loop a few more presented frames
                // while raised before reading pixels back -- see
                // Window::RaiseToTop.
                pendingScreenshot.active =
                    true;

                pendingScreenshot.path =
                    arguments.front();

                pendingScreenshot.
                    framesRemaining = 5;

                return std::string(
                    "OK pending, ~100ms");
            });

        devServer.RegisterCommand(
            "SLEW",
            [&slewState,
             &observer,
             &planet](
                const std::vector<
                    std::string>&
                    arguments)
            {
                if (arguments.size() != 5)
                {
                    return std::string(
                        "ERR usage: SLEW <dirX> <dirY> <dirZ> <altitudeMeters> <durationSeconds>");
                }

                const orbit::math::Double3
                    rawDirection{
                        std::stod(
                            arguments[0]),
                        std::stod(
                            arguments[1]),
                        std::stod(
                            arguments[2])
                    };

                if (orbit::math::Length(
                        rawDirection) <=
                    0.0)
                {
                    return std::string(
                        "ERR direction must be non-zero");
                }

                slewState.fromDirection =
                    orbit::math::Normalize(
                        observer.meters);

                slewState.
                    fromAltitudeMeters =
                        orbit::math::Length(
                            observer.meters) -
                        planet.radiusMeters;

                slewState.toDirection =
                    orbit::math::Normalize(
                        rawDirection);

                slewState.
                    toAltitudeMeters =
                        std::stod(
                            arguments[3]);

                slewState.durationSeconds =
                    (std::max)(
                        std::stod(
                            arguments[4]),
                        0.01);

                slewState.elapsedSeconds =
                    0.0;

                slewState.active = true;

                return std::string("OK");
            });

        devServer.RegisterCommand(
            "TELEPORT",
            [&observerTravelFrame,
             applyObserverPosition](
                const std::vector<
                    std::string>&
                    arguments)
            {
                if (arguments.size() != 4)
                {
                    return std::string(
                        "ERR usage: TELEPORT <dirX> <dirY> <dirZ> <altitudeMeters>");
                }

                const orbit::math::Double3
                    rawDirection{
                        std::stod(
                            arguments[0]),
                        std::stod(
                            arguments[1]),
                        std::stod(
                            arguments[2])
                    };

                if (orbit::math::Length(
                        rawDirection) <=
                    0.0)
                {
                    return std::string(
                        "ERR direction must be non-zero");
                }

                const orbit::math::Double3
                    direction =
                        orbit::math::Normalize(
                            rawDirection);

                applyObserverPosition(
                    direction,
                    std::stod(
                        arguments[3]));

                observerTravelFrame =
                    orbit::world::
                        MakeSurfaceFrame(
                            direction);

                return std::string("OK");
            });

        devServer.RegisterCommand(
            "PLANET_LOD",
            [&wholePlanetLodForced,
             &forcedPlanetLod,
             &uniformPlanet](
                const std::vector<
                    std::string>&
                    arguments)
            {
                if (arguments.size() !=
                    1)
                {
                    return std::string(
                        "ERR usage: PLANET_LOD <AUTO|tier>");
                }

                if (arguments[0] ==
                    "AUTO")
                {
                    wholePlanetLodForced =
                        false;
                    uniformPlanet.RequestLod(
                        -1);
                    return std::string("OK");
                }

                orbit::i32 tier = 0;

                const auto result =
                    std::from_chars(
                        arguments[0].data(),
                        arguments[0].data() +
                            arguments[0].size(),
                        tier);

                if (result.ec !=
                        std::errc{} ||
                    tier < 0 ||
                    tier >
                        static_cast<orbit::i32>(
                            orbit::terrain_stream::
                                kMaximumUniformPlanetLod))
                {
                    return std::string(
                        "ERR usage: PLANET_LOD <AUTO|tier>");
                }

                wholePlanetLodForced = true;
                forcedPlanetLod = tier;
                uniformPlanet.RequestLod(
                    forcedPlanetLod);

                return std::string("OK");
            });

        devServer.RegisterCommand(
            "QUIT",
            [&remoteQuitRequested](
                const auto&)
            {
                remoteQuitRequested = true;
                return std::string("OK");
            });

        devServer.RegisterCommand(
            "DEBUG_OVERLAY",
            [&debugOverlayVisible](
                const std::vector<
                    std::string>&
                    arguments)
            {
                if (arguments.size() !=
                    1)
                {
                    return std::string(
                        "ERR usage: DEBUG_OVERLAY <ON|OFF>");
                }

                if (arguments[0] ==
                    "ON")
                {
                    debugOverlayVisible =
                        true;
                }
                else if (
                    arguments[0] ==
                    "OFF")
                {
                    debugOverlayVisible =
                        false;
                }
                else
                {
                    return std::string(
                        "ERR usage: DEBUG_OVERLAY <ON|OFF>");
                }

                return std::string("OK");
            });

        // Loopback-only equivalent of the F2/L/G/C keys, for
        // automated verification -- <TARGET> is MENU, LOD, FREEZE, or
        // CUT.
        devServer.RegisterCommand(
            "DEBUG_VISUALS",
            [&debugVisualsMenuVisible,
             &debugLodColorEnabled,
             &debugSideCutEnabled,
             &debugGenerationFrozen,
             &terrainPreview](
                const std::vector<
                    std::string>&
                    arguments)
            {
                if (arguments.size() != 2)
                {
                    return std::string(
                        "ERR usage: DEBUG_VISUALS <MENU|LOD|FREEZE|CUT> <ON|OFF>");
                }

                bool value = false;

                if (arguments[1] == "ON")
                {
                    value = true;
                }
                else if (arguments[1] == "OFF")
                {
                    value = false;
                }
                else
                {
                    return std::string(
                        "ERR usage: DEBUG_VISUALS <MENU|LOD|FREEZE|CUT> <ON|OFF>");
                }

                if (arguments[0] == "MENU")
                {
                    debugVisualsMenuVisible = value;
                }
                else if (arguments[0] == "LOD")
                {
                    debugLodColorEnabled = value;
                    terrainPreview.SetDebugVisuals(
                        debugLodColorEnabled,
                        debugSideCutEnabled);
                }
                else if (arguments[0] == "FREEZE")
                {
                    debugGenerationFrozen = value;
                    terrainPreview.SetGenerationFrozen(
                        debugGenerationFrozen);
                }
                else if (arguments[0] == "CUT")
                {
                    debugSideCutEnabled = value;
                    terrainPreview.SetDebugVisuals(
                        debugLodColorEnabled,
                        debugSideCutEnabled);
                }
                else
                {
                    return std::string(
                        "ERR usage: DEBUG_VISUALS <MENU|LOD|FREEZE|CUT> <ON|OFF>");
                }

                return std::string("OK");
            });

        devServer.RegisterCommand(
            "MAP",
            [&mapVisible,
             &window](
                const std::vector<
                    std::string>&
                    arguments)
            {
                if (arguments.size() !=
                    1)
                {
                    return std::string(
                        "ERR usage: MAP <ON|OFF>");
                }

                if (arguments[0] ==
                    "ON")
                {
                    mapVisible = true;
                }
                else if (
                    arguments[0] ==
                    "OFF")
                {
                    mapVisible = false;
                }
                else
                {
                    return std::string(
                        "ERR usage: MAP <ON|OFF>");
                }

                window->SetRelativeMouseMode(
                    !mapVisible);

                return std::string("OK");
            });

        devServer.RegisterCommand(
            "MAP_LAYER",
            [&planetMap](
                const std::vector<
                    std::string>&
                    arguments)
            {
                if (arguments.size() !=
                    1)
                {
                    return std::string(
                        "ERR usage: MAP_LAYER "
                        "<ELEVATION|TECTONICS|BIOMES|TEMPERATURE|"
                        "PRECIPITATION>");
                }

                const auto& name =
                    arguments[0];

                if (name == "ELEVATION")
                {
                    planetMap.SetActiveLayer(
                        orbit::map_render::
                            MapLayer::Elevation);
                }
                else if (
                    name == "TECTONICS")
                {
                    planetMap.SetActiveLayer(
                        orbit::map_render::
                            MapLayer::Tectonics);
                }
                else if (
                    name == "BIOMES")
                {
                    planetMap.SetActiveLayer(
                        orbit::map_render::
                            MapLayer::Biomes);
                }
                else if (
                    name == "TEMPERATURE")
                {
                    planetMap.SetActiveLayer(
                        orbit::map_render::
                            MapLayer::Temperature);
                }
                else if (
                    name == "PRECIPITATION")
                {
                    planetMap.SetActiveLayer(
                        orbit::map_render::
                            MapLayer::Precipitation);
                }
                else
                {
                    return std::string(
                        "ERR usage: MAP_LAYER "
                        "<ELEVATION|TECTONICS|BIOMES|TEMPERATURE|"
                        "PRECIPITATION>");
                }

                return std::string("OK");
            });

        using FrameClock =
            std::chrono::steady_clock;

        auto previousFrameTime =
            FrameClock::now();

        auto previousStatsTime =
            previousFrameTime;

        orbit::camera::FreeCamera
            freeCamera;

        while (window->PumpEvents())
        {
            const auto currentFrameTime =
                FrameClock::now();

            const orbit::f64 deltaSeconds =
                std::clamp(
                    std::chrono::duration<
                        orbit::f64>(
                            currentFrameTime -
                            previousFrameTime)
                        .count(),
                    0.0,
                    0.05);

            previousFrameTime =
                currentFrameTime;

            devServer.Poll();

            if (remoteQuitRequested)
            {
                break;
            }

            // The window's client size is only ever known live via
            // Win32Window::Width()/Height() (there is no WM_SIZE
            // push) -- compare it against the swapchain's own cached
            // size every frame so a resize (including minimize,
            // which reports 0x0) is caught before anything tries to
            // acquire/present against a now-stale swapchain.
            const orbit::u32 windowWidth =
                window->Width();

            const orbit::u32 windowHeight =
                window->Height();

            if (windowWidth == 0 ||
                windowHeight == 0)
            {
                continue;
            }

            if (runtime.ResizeSwapchainToWindow())
            {
                depthTarget =
                    device->CreateTexture({
                        .width = swapchain->Width(),
                        .height = swapchain->Height(),
                        .format =
                            orbit::rhi::TextureFormat::D32_Float,
                        .initialState =
                            orbit::rhi::ResourceState::DepthWrite
                    });
            }

            if (window->KeyDown(
                    orbit::platform::Key::Escape))
            {
                break;
            }

            const bool f3Down =
                window->KeyDown(
                    orbit::platform::Key::F3);

            if (f3Down &&
                !f3PressedLastFrame)
            {
                debugOverlayVisible =
                    !debugOverlayVisible;
            }

            f3PressedLastFrame = f3Down;

            const bool f4Down =
                window->KeyDown(
                    orbit::platform::Key::F4);

            if (f4Down &&
                !f4PressedLastFrame)
            {
                wholePlanetLodForced =
                    !wholePlanetLodForced;

                uniformPlanet.RequestLod(
                    wholePlanetLodForced
                        ? forcedPlanetLod
                        : -1);
            }

            f4PressedLastFrame = f4Down;

            const bool f2Down =
                window->KeyDown(
                    orbit::platform::Key::F2);

            if (f2Down &&
                !f2PressedLastFrame)
            {
                debugVisualsMenuVisible =
                    !debugVisualsMenuVisible;
            }

            f2PressedLastFrame = f2Down;

            // L/G/C only change anything while the F2 menu itself is
            // visible, so they don't steal those letters from normal
            // play.
            if (debugVisualsMenuVisible)
            {
                const bool lDown =
                    window->KeyDown(
                        orbit::platform::Key::L);

                if (lDown && !lPressedLastFrame)
                {
                    debugLodColorEnabled =
                        !debugLodColorEnabled;

                    terrainPreview.SetDebugVisuals(
                        debugLodColorEnabled,
                        debugSideCutEnabled);
                }

                lPressedLastFrame = lDown;

                const bool gDown =
                    window->KeyDown(
                        orbit::platform::Key::G);

                if (gDown && !gPressedLastFrame)
                {
                    debugGenerationFrozen =
                        !debugGenerationFrozen;

                    terrainPreview.SetGenerationFrozen(
                        debugGenerationFrozen);
                }

                gPressedLastFrame = gDown;

                const bool cDown =
                    window->KeyDown(
                        orbit::platform::Key::C);

                if (cDown && !cPressedLastFrame)
                {
                    debugSideCutEnabled =
                        !debugSideCutEnabled;

                    terrainPreview.SetDebugVisuals(
                        debugLodColorEnabled,
                        debugSideCutEnabled);
                }

                cPressedLastFrame = cDown;
            }

            const bool mDown =
                window->KeyDown(
                    orbit::platform::Key::M);

            if (mDown &&
                !mPressedLastFrame)
            {
                mapVisible = !mapVisible;

                // The map is a click-to-select UI, not a look-around
                // view -- show the real cursor while it's open instead
                // of capturing/centering it for camera look.
                window->SetRelativeMouseMode(
                    !mapVisible);
            }

            mPressedLastFrame = mDown;

            if (mapVisible)
            {
                const bool cycleLeft =
                    window->KeyDown(
                        orbit::platform::Key::
                            ArrowLeft);

                const bool cycleRight =
                    window->KeyDown(
                        orbit::platform::Key::
                            ArrowRight);

                if (cycleLeft &&
                    !planetLodDecPressedLastFrame)
                {
                    planetMap.CycleLayer(
                        false);
                }

                if (cycleRight &&
                    !planetLodIncPressedLastFrame)
                {
                    planetMap.CycleLayer(
                        true);
                }

                planetLodDecPressedLastFrame =
                    cycleLeft;
                planetLodIncPressedLastFrame =
                    cycleRight;

                const bool leftClickDown =
                    window->LeftMouseButtonDown();

                if (leftClickDown &&
                    !leftClickPressedLastFrame &&
                    planetMap.Ready())
                {
                    const orbit::math::Double2
                        cursorPixels =
                            window->CursorPositionPixels();

                    const orbit::math::Double2 uv{
                        std::clamp(
                            cursorPixels.x /
                                std::max(
                                    static_cast<orbit::f64>(
                                        swapchain->Width()),
                                    1.0),
                            0.0,
                            1.0),
                        std::clamp(
                            cursorPixels.y /
                                std::max(
                                    static_cast<orbit::f64>(
                                        swapchain->Height()),
                                    1.0),
                            0.0,
                            1.0)
                    };

                    const orbit::math::Double3
                        clickedDirection =
                            orbit::map_render::
                                EquirectDirectionFromUv(
                                    uv);

                    const orbit::f64
                        groundElevationMeters =
                            authoritativeTerrain->
                                Sample({
                                    .unitDirection =
                                        clickedDirection,
                                    .footprintMeters =
                                        20.0
                                }).elevationMeters;

                    constexpr orbit::f64
                        teleportClearanceMeters = 300.0;

                    // A click over open ocean samples the sea BED, which
                    // can be thousands of meters below the nominal
                    // sphere -- floor at sea level (0) first so the
                    // clearance always lands above water, never still
                    // below the planet's base radius (which
                    // TerrainPreviewRenderer/RiverWaterRenderer both
                    // require of the observer).
                    const orbit::f64
                        baseElevationMeters =
                            std::max(
                                groundElevationMeters,
                                0.0);

                    applyObserverPosition(
                        clickedDirection,
                        baseElevationMeters +
                            teleportClearanceMeters);

                    mapVisible = false;
                    window->SetRelativeMouseMode(
                        true);
                }

                leftClickPressedLastFrame =
                    leftClickDown;
            }
            else if (wholePlanetLodForced)
            {
                const bool decDown =
                    window->KeyDown(
                        orbit::platform::Key::
                            ArrowLeft);

                const bool incDown =
                    window->KeyDown(
                        orbit::platform::Key::
                            ArrowRight);

                if (decDown &&
                    !planetLodDecPressedLastFrame &&
                    forcedPlanetLod > 0)
                {
                    --forcedPlanetLod;
                    uniformPlanet.RequestLod(
                        forcedPlanetLod);
                }

                if (incDown &&
                    !planetLodIncPressedLastFrame &&
                    forcedPlanetLod <
                        static_cast<orbit::i32>(
                            orbit::terrain_stream::
                                kMaximumUniformPlanetLod))
                {
                    ++forcedPlanetLod;
                    uniformPlanet.RequestLod(
                        forcedPlanetLod);
                }

                planetLodDecPressedLastFrame =
                    decDown;
                planetLodIncPressedLastFrame =
                    incDown;
            }

            planetMap.Poll();

            uniformPlanet.Poll();

            if (uniformPlanet.HasReadyMesh())
            {
                // CommitReadyMesh replaces GPU buffers the pipeline
                // may still be reading from an in-flight frame --
                // wait for every frame submitted so far first.
                if (nextFenceValue > 1)
                {
                    frameFence->Wait(
                        nextFenceValue - 1);
                }

                uniformPlanet.CommitReadyMesh();
            }

            const orbit::platform::MouseDelta
                mouseDelta =
                    window->ConsumeMouseDelta();

            orbit::f64 moveRight = 0.0;
            orbit::f64 moveForward = 0.0;
            orbit::f64 moveUp = 0.0;

            if (window->KeyDown(
                    orbit::platform::Key::D))
            {
                moveRight += 1.0;
            }

            if (window->KeyDown(
                    orbit::platform::Key::A))
            {
                moveRight -= 1.0;
            }

            if (window->KeyDown(
                    orbit::platform::Key::W))
            {
                moveForward += 1.0;
            }

            if (window->KeyDown(
                    orbit::platform::Key::S))
            {
                moveForward -= 1.0;
            }

            if (window->KeyDown(
                    orbit::platform::Key::E))
            {
                moveUp += 1.0;
            }

            if (window->KeyDown(
                    orbit::platform::Key::Q))
            {
                moveUp -= 1.0;
            }

            const orbit::camera::FreeCameraUpdate
                cameraUpdate =
                    freeCamera.Update({
                        .deltaSeconds =
                            deltaSeconds,
                        .mouseDeltaX =
                            static_cast<orbit::f64>(
                                mouseDelta.x),
                        .mouseDeltaY =
                            static_cast<orbit::f64>(
                                mouseDelta.y),
                        .moveRight =
                            moveRight,
                        .moveForward =
                            moveForward,
                        .moveUp =
                            moveUp,
                        .boost =
                            window->KeyDown(
                                orbit::platform::
                                    Key::LeftShift),
                        .altitudeMeters =
                            lastAltitudeAboveGroundMeters
                    });

            const bool moved =
                cameraUpdate.moved &&
                !slewState.active;

            if (moved)
            {
                const orbit::f64 radius =
                    orbit::math::Length(
                        observer.meters);

                orbit::math::Double3 direction =
                    orbit::math::Normalize(
                        observer.meters);

                if (cameraUpdate.
                        tangentMotionMeters.x !=
                        0.0 ||
                    cameraUpdate.
                        tangentMotionMeters.y !=
                        0.0)
                {
                    lastSurfaceTravelDirection =
                        orbit::math::Normalize(
                            observerTravelFrame.east *
                                cameraUpdate.
                                    tangentMotionMeters.x +
                            observerTravelFrame.north *
                                cameraUpdate.
                                    tangentMotionMeters.y);

                    observerTravelFrame =
                        orbit::world::
                            SurfaceFrameAtOffset(
                                planet,
                                observerTravelFrame,
                                cameraUpdate.
                                    tangentMotionMeters);

                    direction =
                        observerTravelFrame.up;
                }

                applyObserverPosition(
                    direction,
                    radius -
                        planet.radiusMeters +
                        cameraUpdate.
                            verticalMotionMeters);
            }

            if (slewState.active)
            {
                slewState.elapsedSeconds +=
                    deltaSeconds;

                const orbit::f64 t =
                    std::clamp(
                        slewState.
                                elapsedSeconds /
                            slewState.
                                durationSeconds,
                        0.0,
                        1.0);

                // Spherical linear interpolation between the two
                // surface directions so a long slew arcs across the
                // planet instead of cutting a straight line through
                // it.
                const orbit::f64 cosAngle =
                    std::clamp(
                        orbit::math::Dot(
                            slewState.
                                fromDirection,
                            slewState.
                                toDirection),
                        -1.0,
                        1.0);

                const orbit::f64 angle =
                    std::acos(cosAngle);

                orbit::math::Double3
                    slewDirection{};

                if (angle < 1e-9)
                {
                    slewDirection =
                        slewState.
                            toDirection;
                }
                else
                {
                    const orbit::f64
                        sinAngle =
                            std::sin(angle);

                    slewDirection =
                        orbit::math::
                            Normalize(
                                slewState.
                                        fromDirection *
                                    (std::sin(
                                         (1.0 -
                                          t) *
                                         angle) /
                                     sinAngle) +
                                slewState.
                                        toDirection *
                                    (std::sin(
                                         t *
                                         angle) /
                                     sinAngle));
                }

                const orbit::f64
                    slewAltitude =
                        std::lerp(
                            slewState.
                                fromAltitudeMeters,
                            slewState.
                                toAltitudeMeters,
                            t);

                applyObserverPosition(
                    slewDirection,
                    slewAltitude);

                observerTravelFrame =
                    orbit::world::
                        MakeSurfaceFrame(
                            slewDirection);

                lastSurfaceTravelDirection =
                    slewState.toDirection;

                if (t >= 1.0)
                {
                    slewState.active = false;
                }
            }

            regionStreamer.Update(
                orbit::math::Normalize(
                    observer.meters),
                lastSurfaceTravelDirection);

            fineRegionStreamer.Update(
                orbit::math::Normalize(
                    observer.meters),
                lastSurfaceTravelDirection);

            if (debugOverlayVisible)
            {
                const orbit::f64 fps =
                    deltaSeconds > 0.0
                        ? 1.0 / deltaSeconds
                        : 0.0;

                const orbit::f64
                    altitudeMeters =
                        orbit::math::Length(
                            observer.meters) -
                        planet.radiusMeters;

                debugOverlayLines[0]->
                    SetText(
                        std::format(
                            "FPS {:.0f} MS {:.1f}",
                            fps,
                            deltaSeconds *
                                1000.0));

                debugOverlayLines[1]->
                    SetText(
                        std::format(
                            "ALT {:.0f}M",
                            altitudeMeters));
            }

            const orbit::terrain_render::
                TerrainPreviewCamera camera{
                    .forward =
                        cameraUpdate.forward,
                    .up =
                        cameraUpdate.up
                };

            const orbit::u32 frameIndex =
                swapchain->CurrentBackBufferIndex();

            const orbit::u64 pendingFence =
                frameFenceValues[frameIndex];

            if (pendingFence != 0)
            {
                frameFence->Wait(pendingFence);

                // Safe precisely because this frame slot's fence (just
                // waited above) guards both its allocator reuse and
                // these same timestamp queries -- the GPU work that
                // wrote them last time this slot came around is
                // guaranteed complete.
                const orbit::u32 timestampBase =
                    frameIndex * kTimestampsPerFrame;

                std::array<orbit::u64, kTimestampsPerFrame>
                    ticks{};

                if (gpuTimestamps->TryGetResults(
                        timestampBase,
                        kTimestampsPerFrame,
                        ticks.data()))
                {
                    lastGpuSceneMs =
                        static_cast<orbit::f64>(
                            ticks[1] - ticks[0]) *
                        timestampPeriodNs /
                        1'000'000.0;

                    lastGpuOverlayMs =
                        static_cast<orbit::f64>(
                            ticks[3] - ticks[2]) *
                        timestampPeriodNs /
                        1'000'000.0;
                }
            }

            auto& allocator =
                *frameAllocators[frameIndex];

            allocator.Reset();
            commandList->Reset(allocator);

            commandList->ResetTimestampQueryPool(
                *gpuTimestamps,
                frameIndex * kTimestampsPerFrame,
                kTimestampsPerFrame);

            // Records this frame's ground-elevation dispatches (if
            // any are queued) and promotes previously-dispatched ones
            // whose fence value has now retired -- nextFenceValue here
            // is exactly the value this frame's Signal() call below
            // will use (see signalValue's assignment).
            gpuElevationQuery.Flush(
                *commandList,
                nextFenceValue);

            // Same pattern -- dispatches at most one queued region
            // tile's GPU hydrology build this frame, and reads back
            // any previously-dispatched one whose fence value has now
            // retired (see DerivedTerrainRegionCacheConfig::gpuHydrology).
            regionCache->Flush(
                *commandList,
                nextFenceValue);

            fineRegionCache->Flush(
                *commandList,
                nextFenceValue);

            auto& backBuffer =
                swapchain->CurrentBackBuffer();

            commandList->Transition(
                backBuffer,
                orbit::rhi::ResourceState::Present,
                orbit::rhi::ResourceState::RenderTarget);

            commandList->ClearColorTarget(
                backBuffer,
                {
                    .red = 0.008F,
                    .green = 0.012F,
                    .blue = 0.020F,
                    .alpha = 1.0F
                });

            commandList->ClearDepthTarget(
                *depthTarget,
                0.0F);

            commandList->SetRenderTargets(
                backBuffer,
                *depthTarget);

            const orbit::u32 timestampBase =
                frameIndex * kTimestampsPerFrame;

            commandList->WriteTimestamp(
                *gpuTimestamps,
                timestampBase + 0);

            const bool drawWholePlanetLod =
                wholePlanetLodForced &&
                uniformPlanet.ActiveLod() >= 0;

            if (mapVisible)
            {
                planetMap.Draw(
                    *commandList,
                    backBuffer,
                    swapchain->Width(),
                    swapchain->Height());
            }
            else if (drawWholePlanetLod)
            {
                uniformPlanet.Draw(
                    *commandList,
                    observer,
                    orbit::world::MakeSurfaceFrame(
                        orbit::math::Normalize(
                            observer.meters)),
                    swapchain->Width(),
                    swapchain->Height(),
                    camera);
            }
            else
            {
                terrainPreview.Draw(
                    *commandList,
                    frameIndex,
                    swapchain->Width(),
                    swapchain->Height(),
                    camera);

                riverWater.Draw(
                    *commandList,
                    backBuffer,
                    *depthTarget,
                    frameIndex,
                    swapchain->Width(),
                    swapchain->Height(),
                    {
                        .forward =
                            camera.forward,
                        .up =
                            camera.up
                    });
            }

            commandList->WriteTimestamp(
                *gpuTimestamps,
                timestampBase + 1);

            commandList->WriteTimestamp(
                *gpuTimestamps,
                timestampBase + 2);

            versionOverlay.Draw(
                *commandList,
                backBuffer,
                swapchain->Width(),
                swapchain->Height());

            // Names the active map layer (ELEVATION, TECTONICS, ...) so a
            // screenshot of the map is self-identifying -- shown whenever
            // the map is on screen, independent of the F3 debug HUD below.
            if (mapVisible)
            {
                debugOverlayLines[8]->SetText(
                    std::format(
                        "MAP {}",
                        MapLayerName(
                            planetMap.ActiveLayer())));

                debugOverlayLines[8]->Draw(
                    *commandList,
                    backBuffer,
                    swapchain->Width(),
                    swapchain->Height());

                // Color legend for the Tectonics layer -- the boundary
                // colors alone (see TectonicsColor in PlanetMapRenderer.cpp)
                // don't say which plate-tectonics subtype they mean. Uses
                // "-" (not "=") and stays under 32 chars/line: the debug
                // overlay font only supports A-Z 0-9 space . - : and
                // silently truncates past that, which "=" and two longer
                // combined lines both hit.
                if (planetMap.ActiveLayer() ==
                    orbit::map_render::MapLayer::Tectonics)
                {
                    debugOverlayLines[9]->SetText(
                        "RED-OROGENY ORANGE-SUBDUCTION");
                    debugOverlayLines[10]->SetText(
                        "MAGENTA-HOTSPOT CYAN-RIDGE");
                    debugOverlayLines[11]->SetText(
                        "GREEN-RIFT YELLOW-TRANSFORM");

                    debugOverlayLines[9]->Draw(
                        *commandList,
                        backBuffer,
                        swapchain->Width(),
                        swapchain->Height());

                    debugOverlayLines[10]->Draw(
                        *commandList,
                        backBuffer,
                        swapchain->Width(),
                        swapchain->Height());

                    debugOverlayLines[11]->Draw(
                        *commandList,
                        backBuffer,
                        swapchain->Width(),
                        swapchain->Height());
                }
            }

            if (debugOverlayVisible)
            {
                const auto& rebase = terrainPreview.StreamingStats();
                debugOverlayLines[5]->SetText(rebase.rebaseCount == 0
                    ? "REBASE 0 NONE"
                    : std::format("REBASE {} {} {} LVL {:.1f}S {}",
                        rebase.rebaseCount, rebase.lastRebaseReason,
                        rebase.lastRebaseLevels, rebase.secondsSinceLastRebase,
                        rebase.secondsSinceLastRebase < 2.0 ? "NOW" : ""));

                debugOverlayLines[6]->SetText(
                    wholePlanetLodForced
                        ? std::format(
                            "PLANET LOD {}{}",
                            forcedPlanetLod,
                            uniformPlanet.Building()
                                ? " BUILDING"
                                : "")
                        : "PLANET LOD AUTO");

                debugOverlayLines[7]->SetText(
                    std::format(
                        "GPU SCENE {:.2f}MS OVERLAY {:.2f}MS",
                        lastGpuSceneMs,
                        lastGpuOverlayMs));

                // Line 8 (the map layer name) is drawn separately above,
                // gated on mapVisible instead of this F3 toggle.
                for (orbit::u32 lineIndex = 0;
                     lineIndex < 8;
                     ++lineIndex)
                {
                    debugOverlayLines[lineIndex]->Draw(
                        *commandList,
                        backBuffer,
                        swapchain->Width(),
                        swapchain->Height());
                }
            }

            if (debugVisualsMenuVisible)
            {
                debugVisualsLines[0]->SetText(
                    "F2 DEBUG VISUALS MENU");

                debugVisualsLines[1]->SetText(
                    std::format(
                        "L LOD COLOR {}",
                        debugLodColorEnabled ? "ON" : "OFF"));

                debugVisualsLines[2]->SetText(
                    std::format(
                        "G FREEZE GEN {}",
                        debugGenerationFrozen ? "ON" : "OFF"));

                debugVisualsLines[3]->SetText(
                    std::format(
                        "C SIDE CUT {}",
                        debugSideCutEnabled ? "ON" : "OFF"));

                for (orbit::u32 lineIndex = 0;
                     lineIndex < kDebugVisualsLineCount;
                     ++lineIndex)
                {
                    debugVisualsLines[lineIndex]->Draw(
                        *commandList,
                        backBuffer,
                        swapchain->Width(),
                        swapchain->Height());
                }
            }

            commandList->WriteTimestamp(
                *gpuTimestamps,
                timestampBase + 3);

            commandList->Transition(
                backBuffer,
                orbit::rhi::ResourceState::RenderTarget,
                orbit::rhi::ResourceState::Present);

            commandList->Close();
            graphicsQueue->Submit(*commandList);

            swapchain->Present(true);

            const orbit::u64 signalValue =
                nextFenceValue++;

            graphicsQueue->Signal(
                *frameFence,
                signalValue);

            frameFenceValues[frameIndex] =
                signalValue;

            if (pendingScreenshot.active)
            {
                // Wait for a handful of frames to actually present
                // while the window is raised before reading the
                // screen back -- see Window::RaiseToTop.
                if (--pendingScreenshot.
                        framesRemaining <=
                    0)
                {
                    if (!window->
                            CaptureScreenshotBmp(
                                pendingScreenshot.
                                    path))
                    {
                        orbit::log::Warning(
                            std::format(
                                "Orbit dev server screenshot to '{}' failed.",
                                pendingScreenshot.
                                    path));
                    }

                    pendingScreenshot.active =
                        false;
                }
            }

            if (currentFrameTime -
                    previousStatsTime >=
                std::chrono::seconds(1))
            {
                // Drop cached pages that have fallen far behind the
                // observer instead of only ever evicting once the
                // byte budget is already full -- see
                // TerrainPageCache::PruneFarPages. Once a second is
                // plenty; this walks every resident entry.
                terrain.PruneFarPages(
                    orbit::math::Normalize(
                        observer.meters));

                const auto& stats =
                    terrainPreview.
                        StreamingStats();

                const auto cacheStats =
                    terrain.Stats();

                const auto pageCacheStats =
                    terrain.PageCacheStats();

                const auto regionStats =
                    regionCache->Stats();

                const auto& regionStreamStats =
                    regionStreamer.Stats();

                const auto fineRegionStats =
                    fineRegionCache->Stats();


                const auto& waterStats =
                    riverWater.Stats();

                constexpr orbit::f64 bytesPerMiB =
                    1024.0 * 1024.0;

                lastStatsLine =
                    std::format(
                        "alt_km={:.0f} samples={} levels={} regions={} upload_b={} draws={} clip_tier={} spacing_m={:.0f} radius_km={:.0f} page_mib={:.1f}/{:.0f} entries={} evict={} reject={} derived_ready={} pending={} desired={} requests={} fine_ready={} fine_pending={} revisions={} stale={} rebases={} rebase_reason={} rebase_levels={} rebase_age_s={:.2f} standing_water=terrain rivers={} legacy_lake_cells={} water_upload_b={}",
                        (orbit::math::Length(
                            observer.meters) -
                         planet.radiusMeters) /
                            1000.0,
                        stats.
                            generatedSamplesLastUpdate,
                        stats.
                            levelsTouchedLastUpdate,
                        stats.
                            refreshedRegionsLastUpdate,
                        stats.
                            uploadedBytesLastFrame,
                        stats.
                            drawCallsLastFrame,
                        stats.
                            adaptiveCoverageTier,
                        stats.
                            activeBaseSpacingMeters,
                        stats.
                            activeOuterHalfExtentMeters /
                            1000.0,
                        static_cast<orbit::f64>(
                            pageCacheStats.
                                residentBytes) /
                            bytesPerMiB,
                        static_cast<orbit::f64>(
                            pageCacheStats.
                                budgetBytes) /
                            bytesPerMiB,
                        pageCacheStats.entries,
                        pageCacheStats.evictions,
                        pageCacheStats.
                            capacityRejects,
                        regionStats.readyEntries,
                        regionStats.pendingEntries,
                        regionStreamStats.
                            desiredRegions,
                        regionStats.
                            acceptedRequests,
                        fineRegionStats.
                            readyEntries,
                        fineRegionStats.
                            pendingEntries,
                        stats.
                            revisionInvalidations,
                        stats.
                            staleRevisionBatches,
                        stats.rebaseCount,
                        stats.lastRebaseReason,
                        stats.lastRebaseLevels,
                        stats.secondsSinceLastRebase,
                        waterStats.
                            visibleSegmentsLastFrame,
                        waterStats.
                            visibleLakeCellsLastFrame,
                        waterStats.
                            uploadedBytesLastFrame);

                if (debugOverlayVisible)
                {
                    debugOverlayLines[2]->
                        SetText(
                            std::format(
                                "SAMP {} LVL {} DRAW {}",
                                stats.
                                    generatedSamplesLastUpdate,
                                stats.
                                    levelsTouchedLastUpdate,
                                stats.
                                    drawCallsLastFrame));

                    debugOverlayLines[3]->
                        SetText(
                            std::format(
                                "PAGE {:.0f}M CAP {:.0f}M EV {}",
                                static_cast<
                                    orbit::f64>(
                                    pageCacheStats.
                                        residentBytes) /
                                    bytesPerMiB,
                                static_cast<
                                    orbit::f64>(
                                    pageCacheStats.
                                        budgetBytes) /
                                    bytesPerMiB,
                                pageCacheStats.
                                    evictions));

                    debugOverlayLines[4]->
                        SetText(
                            std::format(
                                "WATER TERRAIN RIV {}",
                                waterStats.visibleSegmentsLastFrame));
                }

                orbit::log::Info(
                    std::format(
                        "Terrain stream | alt {:.0f} km | samples {} levels {} regions {} | upload {} B | draws {} | clip tier {} spacing {:.0f} m radius {:.0f} km | page {:.1f}/{:.0f} MiB entries {} evict {} reject {} | derived ready {} pending {} desired {} requests {} | fine ready {} pending {} | revisions {} stale {} | standing water: terrain | water rivers {} legacy lake cells {} upload {} B",
                        (orbit::math::Length(
                            observer.meters) -
                         planet.radiusMeters) /
                            1000.0,
                        stats.
                            generatedSamplesLastUpdate,
                        stats.
                            levelsTouchedLastUpdate,
                        stats.
                            refreshedRegionsLastUpdate,
                        stats.
                            uploadedBytesLastFrame,
                        stats.
                            drawCallsLastFrame,
                        stats.
                            adaptiveCoverageTier,
                        stats.
                            activeBaseSpacingMeters,
                        stats.
                            activeOuterHalfExtentMeters /
                            1000.0,
                        static_cast<orbit::f64>(
                            pageCacheStats.
                                residentBytes) /
                            bytesPerMiB,
                        static_cast<orbit::f64>(
                            pageCacheStats.
                                budgetBytes) /
                            bytesPerMiB,
                        pageCacheStats.entries,
                        pageCacheStats.evictions,
                        pageCacheStats.
                            capacityRejects,
                        regionStats.readyEntries,
                        regionStats.pendingEntries,
                        regionStreamStats.
                            desiredRegions,
                        regionStats.
                            acceptedRequests,
                        fineRegionStats.
                            readyEntries,
                        fineRegionStats.
                            pendingEntries,
                        stats.
                            revisionInvalidations,
                        stats.
                            staleRevisionBatches,
                        waterStats.
                            visibleSegmentsLastFrame,
                        waterStats.
                            visibleLakeCellsLastFrame,
                        waterStats.
                            uploadedBytesLastFrame));

                previousStatsTime =
                    currentFrameTime;
            }
        }

        const orbit::u64 shutdownFence =
            nextFenceValue++;

        graphicsQueue->Signal(
            *frameFence,
            shutdownFence);

        frameFence->Wait(shutdownFence);

        orbit::log::Info("Orbit shutdown.");
        return 0;
    }
    catch (const std::exception& exception)
    {
        orbit::log::Error(exception.what());
        return 1;
    }
}
