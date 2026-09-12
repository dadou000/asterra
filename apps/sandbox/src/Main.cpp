#include <orbit/camera/FreeCamera.hpp>
#include <orbit/core/BuildInfo.hpp>
#include <orbit/core/Log.hpp>
#include <orbit/debug_render/VersionOverlayRenderer.hpp>
#include <orbit/jobs/JobSystem.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/platform/CrashHandler.hpp>
#include <orbit/platform/Window.hpp>
#include <orbit/rhi/d3d12/D3D12Backend.hpp>
#include <orbit/shader/d3d/D3DShaderCompiler.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain_cache/CachedTerrainSource.hpp>
#include <orbit/terrain_region/DerivedRegionTerrainSource.hpp>
#include <orbit/terrain_region/DerivedTerrainRegionCache.hpp>
#include <orbit/terrain_region/DerivedTerrainRegionStreamer.hpp>
#include <orbit/terrain_render/TerrainPreviewRenderer.hpp>
#include <orbit/terrain_stream/TerrainSampleStreamer.hpp>
#include <orbit/water_render/OceanRenderer.hpp>
#include <orbit/water_render/RiverWaterRenderer.hpp>
#include <orbit/world/Planet.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <format>
#include <memory>
#include <vector>

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

        auto device = orbit::rhi::d3d12::CreateDevice({
            .enableValidation = enableValidation
        });

        const auto& capabilities = device->Capabilities();

        orbit::log::Info(std::format(
            "GPU: {} | SM {}.{} | RT: {} | Mesh shaders: {} | VRS: {} | Tearing: {}",
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

        const orbit::world::PlanetDefinition planet{
            .radiusMeters = 6'000'000.0
        };

        const orbit::math::Double3 observerDirection =
            orbit::math::Normalize(
                orbit::math::Double3{
                    0.65,
                    0.35,
                    0.68
                });

        const orbit::world::SurfaceFrame
            initialSurfaceFrame =
                orbit::world::MakeSurfaceFrame(
                    observerDirection);

        const orbit::terrain::AnalyticTerrainDesc
            terrainDescription{
                .seed = 0xA57E22AULL,
                .macroAmplitudeMeters = 4'000.0,
                .macroWavelengthMeters = 800'000.0,
                .detailAmplitudeMeters = 1'100.0,
                .detailWavelengthMeters = 90'000.0,
                .detailOctaves = 8
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
                                    .generatorVersion = 1,
                                    .overlapScale = 1.35
                                }
                            });

        const auto streamedTerrain =
            std::make_shared<
                orbit::terrain_region::
                    DerivedRegionTerrainSource>(
                        planet,
                        authoritativeTerrain,
                        regionCache);

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
                            256ULL * 1024ULL * 1024ULL,
                        .softEntryLimit =
                            4096
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

        orbit::world::WorldPosition observer{
            .meters =
                observerDirection *
                (planet.radiusMeters + 8'000.0)
        };

        orbit::world::SurfaceFrame
            observerTravelFrame =
                initialSurfaceFrame;

        orbit::math::Double3
            lastSurfaceTravelDirection{};

        const orbit::shader::d3d::D3DShaderCompiler
            shaderCompiler;

        orbit::debug_render::VersionOverlayRenderer
            versionOverlay(
                *device,
                shaderCompiler,
                orbit::build::DisplayVersion);

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

        orbit::water_render::OceanRenderer
            ocean(
                *device,
                shaderCompiler,
                planet,
                observer,
                {
                    .mesh = {
                        .radialRings = 112,
                        .angularSegments = 128
                    },
                    .seaLevelMeters =
                        terrainDescription.
                            global.
                            seaLevelMeters,
                    .minimumRadiusMeters = 25.0,
                    .maximumRadiusMeters =
                        240'000.0,
                    .waveAmplitudeScale = 1.0F,
                    .verticalFovRadians =
                        terrainPreviewConfig.
                            verticalFovRadians,
                    .nearPlaneMeters =
                        terrainPreviewConfig.
                            nearPlaneMeters,
                    .farPlaneMeters =
                        terrainPreviewConfig.
                            farPlaneMeters
                });

        orbit::water_render::RiverWaterRenderer
            riverWater(
                *device,
                shaderCompiler,
                planet,
                regionCache,
                observer,
                {
                    .framesInFlight =
                        swapchain->BufferCount(),
                    .maximumSegments = 16'384,
                    .maximumLakeCells = 8'192,
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

            if (window->KeyDown(
                    orbit::platform::Key::Escape))
            {
                break;
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
                                    Key::LeftShift)
                    });

            const bool moved =
                cameraUpdate.moved;

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

                const orbit::f64 altitude =
                    std::clamp(
                        radius -
                            planet.radiusMeters +
                            cameraUpdate.
                                verticalMotionMeters,
                        250.0,
                        100'000.0);

                observer.meters =
                    direction *
                    (planet.radiusMeters +
                     altitude);

                terrainPreview.UpdateObserver(
                    observer);

                ocean.UpdateObserver(
                    observer);

                riverWater.UpdateObserver(
                    observer);
            }

            regionStreamer.Update(
                orbit::math::Normalize(
                    observer.meters),
                lastSurfaceTravelDirection);

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
            }

            auto& allocator =
                *frameAllocators[frameIndex];

            allocator.Reset();
            commandList->Reset(allocator);

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
                1.0F);

            commandList->SetRenderTargets(
                backBuffer,
                *depthTarget);

            terrainPreview.Draw(
                *commandList,
                frameIndex,
                swapchain->Width(),
                swapchain->Height(),
                camera);

            ocean.Draw(
                *commandList,
                backBuffer,
                *depthTarget,
                swapchain->Width(),
                swapchain->Height(),
                {
                    .forward =
                        camera.forward,
                    .up =
                        camera.up
                });

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

            versionOverlay.Draw(
                *commandList,
                backBuffer,
                swapchain->Width(),
                swapchain->Height());

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

            if (currentFrameTime -
                    previousStatsTime >=
                std::chrono::seconds(1))
            {
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

                const auto& oceanStats =
                    ocean.Stats();

                const auto& waterStats =
                    riverWater.Stats();

                constexpr orbit::f64 bytesPerMiB =
                    1024.0 * 1024.0;

                orbit::log::Info(
                    std::format(
                        "Terrain stream | samples {} levels {} regions {} | upload {} B | draws {} | page {:.1f}/{:.0f} MiB entries {} evict {} reject {} | derived ready {} pending {} desired {} requests {} | revisions {} stale {} | ocean {}v/{}i {} draw | water rivers {} lakes {} upload {} B",
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
                        stats.
                            revisionInvalidations,
                        stats.
                            staleRevisionBatches,
                        oceanStats.vertices,
                        oceanStats.indices,
                        oceanStats.
                            drawCallsLastFrame,
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
