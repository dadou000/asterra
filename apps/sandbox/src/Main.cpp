#include <orbit/core/Log.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/platform/Window.hpp>
#include <orbit/rhi/d3d12/D3D12Backend.hpp>
#include <orbit/shader/d3d/D3DShaderCompiler.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain_render/TerrainPreviewRenderer.hpp>
#include <orbit/world/Planet.hpp>

#include <exception>
#include <format>
#include <memory>
#include <vector>

int main()
{
    try
    {
        orbit::log::Info("Orbit M0 boot.");

        auto window = orbit::platform::MakeWindow({
            .title = "Orbit - Asterra Engine",
            .width = 1600,
            .height = 900
        });

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

        const orbit::terrain::AnalyticTerrainSource terrain(
            planet,
            {
                .seed = 0xA57E22AULL,
                .macroAmplitudeMeters = 4'000.0,
                .macroWavelengthMeters = 800'000.0,
                .detailAmplitudeMeters = 1'100.0,
                .detailWavelengthMeters = 90'000.0,
                .detailOctaves = 8
            });

        const orbit::math::Double3 observerDirection =
            orbit::math::Normalize(orbit::math::Double3{
                0.65,
                0.35,
                0.68
            });

        const orbit::world::WorldPosition observer{
            .meters =
                observerDirection *
                (planet.radiusMeters + 3'500.0)
        };

        const orbit::shader::d3d::D3DShaderCompiler
            shaderCompiler;

        orbit::terrain_render::TerrainPreviewRenderer
            terrainPreview(
                *device,
                shaderCompiler,
                planet,
                terrain,
                observer);

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

        while (window->PumpEvents())
        {
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
