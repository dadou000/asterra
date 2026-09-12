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
#include <orbit/terrain_erosion/HydrologyRefinement.hpp>
#include <orbit/terrain_erosion/RegionalElevationDelta.hpp>
#include <orbit/terrain_erosion/RiverCarvedTerrainSource.hpp>
#include <orbit/terrain_erosion/RiverCarving.hpp>
#include <orbit/terrain_hydrology/HydrologyGrid.hpp>
#include <orbit/terrain_hydrology/RiverGraph.hpp>
#include <orbit/terrain_render/TerrainPreviewRenderer.hpp>
#include <orbit/terrain_stream/TerrainSampleStreamer.hpp>
#include <orbit/world/Planet.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <format>
#include <memory>
#include <utility>
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

        const auto authoritativeTerrain =
            std::make_shared<
                orbit::terrain::AnalyticTerrainSource>(
                    planet,
                    orbit::terrain::AnalyticTerrainDesc{
                        .seed = 0xA57E22AULL,
                        .macroAmplitudeMeters = 4'000.0,
                        .macroWavelengthMeters = 800'000.0,
                        .detailAmplitudeMeters = 1'100.0,
                        .detailWavelengthMeters = 90'000.0,
                        .detailOctaves = 8
                    });

        auto initialHydrology =
            orbit::terrain_hydrology::
                BuildHydrologyGrid(
                    planet,
                    *authoritativeTerrain,
                    initialSurfaceFrame,
                    {
                        .resolution = 129,
                        .halfExtentMeters =
                            250'000.0,
                        .footprintMeters = 0.0,
                        .useCoarseElevation =
                            false,
                        .conditionDepressions =
                            true,
                        .minimumDrainageDropMeters =
                            0.25
                    });

        const auto hydrologyRefinement =
            orbit::terrain_erosion::
                RefineHydrologyWithSediment(
                    std::move(
                        initialHydrology),
                    {
                        .iterations = 2,
                        .elevationDeltaScale =
                            0.35
                    });

        const auto& regionalHydrology =
            hydrologyRefinement.
                hydrology;

        auto regionalDeltaField =
            orbit::terrain_erosion::
                BuildRegionalElevationDeltaField(
                    regionalHydrology,
                    hydrologyRefinement.
                        cumulativeElevationDeltaMeters);

        std::vector<
            orbit::terrain_erosion::
                RegionalElevationDeltaField>
            regionalDeltaFields;

        regionalDeltaFields.push_back(
            std::move(
                regionalDeltaField));

        const auto regionallyErodedTerrain =
            std::make_shared<
                orbit::terrain_erosion::
                    RegionalElevationDeltaTerrainSource>(
                        planet,
                        authoritativeTerrain,
                        std::move(
                            regionalDeltaFields),
                        orbit::terrain_erosion::
                            RegionalElevationDeltaConfig{
                                .regionEdgeFadeMeters =
                                    25'000.0,
                                .fullDetailFootprintScale =
                                    0.5,
                                .fadeOutFootprintScale =
                                    4.0
                            });

        const auto riverGraph =
            orbit::terrain_hydrology::
                BuildRiverGraph(
                    regionalHydrology,
                    400'000'000.0);

        auto carvingField =
            orbit::terrain_erosion::
                BuildRiverCarvingField(
                    regionalHydrology,
                    riverGraph,
                    {
                        .referenceDrainageAreaSquareMeters =
                            1'000'000'000.0,
                        .baseChannelHalfWidthMeters =
                            20.0,
                        .minimumChannelHalfWidthMeters =
                            5.0,
                        .maximumChannelHalfWidthMeters =
                            180.0,
                        .widthExponent =
                            0.30,
                        .baseDepthMeters =
                            8.0,
                        .minimumDepthMeters =
                            2.0,
                        .maximumDepthMeters =
                            80.0,
                        .depthExponent =
                            0.18,
                        .valleyWidthMultiplier =
                            8.0,
                        .minimumBedSlope =
                            0.00008,
                        .maximumIncisionMeters =
                            250.0,
                        .spatialIndexResolution =
                            64
                    });

        orbit::log::Info(
            std::format(
                "Regional terrain | hydrology {}x{} | river nodes {} segments {} | erosion index refs {} | exported sediment {:.2f}",
                regionalHydrology.
                    config.resolution,
                regionalHydrology.
                    config.resolution,
                riverGraph.nodes.size(),
                riverGraph.segments.size(),
                carvingField.
                    spatialSegmentIndices.
                    size(),
                hydrologyRefinement.
                    lastSediment.
                    exportedSediment));

        std::vector<
            orbit::terrain_erosion::
                RiverCarvingField>
            carvingFields;

        carvingFields.push_back(
            std::move(
                carvingField));

        const auto carvedTerrain =
            std::make_shared<
                orbit::terrain_erosion::
                    RiverCarvedTerrainSource>(
                        planet,
                        regionallyErodedTerrain,
                        std::move(
                            carvingFields));

        orbit::jobs::JobSystem jobSystem;

        orbit::terrain_cache::CachedTerrainSource
            terrain(
                planet,
                carvedTerrain,
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

            if (moved &&
                currentFrameTime -
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

                orbit::log::Info(
                    std::format(
                        "Terrain stream | generated {} samples across {} levels / {} regions | uploaded {} bytes | {} draws | cache hits {} fallback {} requests {} resident {} evictions {} rejects {}",
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
                        cacheStats.pageHits,
                        cacheStats.
                            directFallbackSamples,
                        cacheStats.pageRequests,
                        pageCacheStats.entries,
                        pageCacheStats.evictions,
                        pageCacheStats.
                            capacityRejects));

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
