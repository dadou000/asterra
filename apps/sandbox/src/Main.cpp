#include <orbit/camera/FreeCamera.hpp>
#include <orbit/core/BuildInfo.hpp>
#include <orbit/core/Log.hpp>
#include <orbit/debug_render/VersionOverlayRenderer.hpp>
#include <orbit/dev_server/DevServer.hpp>
#include <orbit/jobs/JobSystem.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/platform/CrashHandler.hpp>
#include <orbit/platform/Window.hpp>
#include <orbit/rhi/vulkan/VulkanBackend.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain_cache/CachedTerrainSource.hpp>
#include <orbit/terrain_region/DerivedRegionTerrainSource.hpp>
#include <orbit/terrain_region/DerivedTerrainRegionCache.hpp>
#include <orbit/terrain_region/DerivedTerrainRegionStreamer.hpp>
#include <orbit/terrain_render/TerrainPreviewRenderer.hpp>
#include <orbit/terrain_render/UniformPlanetRenderer.hpp>
#include <orbit/terrain_stream/TerrainSampleStreamer.hpp>
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
#include <vector>

namespace
{
// Environment-variable opt-ins for performance-analysis tooling (extra
// Vulkan validation-layer features, RenderDoc) -- see their call site.
// Any non-empty value counts as enabled; unset counts as disabled.
[[nodiscard]] bool EnvFlagEnabled(const char* name)
{
    char* value = nullptr;
    std::size_t valueLength = 0;

    const bool found =
        _dupenv_s(&value, &valueLength, name) == 0 && value != nullptr;

    if (found)
    {
        free(value);
    }

    return found;
}
} // namespace

int main()
{
    const bool crashHandlerInstalled =
        orbit::platform::InstallCrashHandler({
            .applicationName = "OrbitSandbox",
            .writeMiniDump = true
        });

    try
    {
        if (!crashHandlerInstalled)
        {
            orbit::log::Warning(
                "Orbit crash handler could not be installed.");
        }

        orbit::log::Info(
            std::format(
                "Orbit M0 boot | {}",
                orbit::build::DisplayVersion));

        auto window = orbit::platform::MakeWindow({
            .title = "Orbit - Asterra Engine",
            .width = 1600,
            .height = 900
        });

        window->SetRelativeMouseMode(true);

        orbit::log::Info(
            "Camera controls: WASD move, mouse look, Q/E down/up, Shift boost, Esc quit.");

#if defined(NDEBUG)
        constexpr bool enableValidation = false;
#else
        constexpr bool enableValidation = true;
#endif

        // Extra validation-layer features and RenderDoc are opt-in via
        // environment variables rather than compiled in, so they can be
        // toggled per-run for a performance-analysis session without a
        // rebuild -- e.g. turn on synchronization validation to hunt a
        // bug, then back off to get accurate timing again.
        const bool enableBestPractices =
            EnvFlagEnabled("ORBIT_VK_BEST_PRACTICES");
        const bool enableSyncValidation =
            EnvFlagEnabled("ORBIT_VK_SYNC_VALIDATION");
        const bool enableGpuAssisted =
            EnvFlagEnabled("ORBIT_VK_GPU_ASSISTED");
        const bool enableRenderDoc =
            EnvFlagEnabled("ORBIT_RENDERDOC");

        auto device = orbit::rhi::vulkan::CreateDevice({
            .enableValidation =
                enableValidation ||
                enableBestPractices ||
                enableSyncValidation ||
                enableGpuAssisted,
            .enableBestPracticesValidation = enableBestPractices,
            .enableSynchronizationValidation = enableSyncValidation,
            .enableGpuAssistedValidation = enableGpuAssisted,
            .enableRenderDoc = enableRenderDoc
        });

        if (enableRenderDoc)
        {
            orbit::rhi::vulkan::SetRenderDocActiveWindow(
                *device,
                window->NativeHandle());
        }

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

        auto graphicsQueue =
            device->CreateQueue(
                orbit::rhi::QueueType::Graphics);

        auto swapchain =
            device->CreateSwapchain(
                *graphicsQueue,
                {
                    .nativeWindow =
                        window->NativeHandle(),
                    .width = window->Width(),
                    .height = window->Height(),
                    .bufferCount = 3,
                    .allowTearing = true
                });

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

        const orbit::world::PlanetDefinition planet{
            .radiusMeters = 6'000'000.0
        };

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
                                }
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
                                }
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

        orbit::terrain_stream::TerrainSampleStreamer
            terrainSampleStreamer(
                jobSystem,
                planet,
                terrain);

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

        const orbit::shader::dxc::DxcShaderCompiler
            shaderCompiler;

        orbit::debug_render::VersionOverlayRenderer
            versionOverlay(
                *device,
                shaderCompiler,
                orbit::build::DisplayVersion);

        // F3 debug HUD: a stack of small text panels pinned to the
        // top-left corner, updated live and toggled at runtime.
        constexpr orbit::u32
            kDebugOverlayLineCount = 8;

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

        bool wholePlanetLodForced = false;
        orbit::i32 forcedPlanetLod = 0;
        bool f4PressedLastFrame = false;
        bool planetLodDecPressedLastFrame = false;
        bool planetLodIncPressedLastFrame = false;

        orbit::terrain_render::TerrainPreviewConfig
            terrainPreviewConfig{};

        terrainPreviewConfig.framesInFlight =
            swapchain->BufferCount();

        orbit::terrain_render::TerrainPreviewRenderer
            terrainPreview(
                *device,
                shaderCompiler,
                planet,
                terrainSampleStreamer,
                observer,
                terrainPreviewConfig);

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
        const auto applyObserverPosition =
            [&observer,
             &planet,
             authoritativeTerrain,
             &lastAltitudeAboveGroundMeters,
             &terrainPreview,
             &riverWater](
                const orbit::math::Double3&
                    direction,
                const orbit::f64
                    desiredAltitudeMeters)
        {
            const orbit::terrain::TerrainSample
                groundSample =
                    authoritativeTerrain->
                        Sample({
                            .unitDirection =
                                direction,
                            .footprintMeters =
                                10.0
                        });

            constexpr orbit::f64
                minClearanceMeters = 2.0;

            const orbit::f64
                minAltitudeMeters =
                    groundSample.
                        elevationMeters +
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
                groundSample.
                    elevationMeters;

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

            if (windowWidth != swapchain->Width() ||
                windowHeight != swapchain->Height())
            {
                swapchain->Resize(
                    windowWidth,
                    windowHeight);

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

            if (wholePlanetLodForced)
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

            if (drawWholePlanetLod)
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

                for (const auto& line :
                     debugOverlayLines)
                {
                    line->Draw(
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
