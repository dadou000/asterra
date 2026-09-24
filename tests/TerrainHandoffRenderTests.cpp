#include <orbit/celestial_globe/MacroGlobe.hpp>
#include <orbit/render_view/Capture.hpp>
#include <orbit/rhi/vulkan/VulkanBackend.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain_render/TerrainPreviewRenderer.hpp>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

int main()
{
    using namespace orbit;
    auto device = rhi::vulkan::CreateDevice({.enableValidation = true});
    shader::dxc::DxcShaderCompiler compiler;
    world::PlanetDefinition planet{.radiusMeters = 6'000'000.0};
    terrain::AnalyticTerrainSource source(planet, {.seed = 0xA57E22AULL});
    const auto mesh = celestial_globe::BuildMacroGlobe(source,
        universe::SphereShape{.radiusMeters = planet.radiusMeters}, {.faceResolution = 129});
    const auto appearance = celestial_appearance::BuildPlanetaryAppearance(
        source, planet.radiusMeters, {.faceResolution = 129});
    celestial_globe::GpuMacroGlobeProduct globe(*device, mesh, &appearance);
    celestial_globe::MacroGlobeRenderer globeRenderer(*device, compiler);
    terrain_gpu::GpuFieldGenerator fields(*device, compiler, planet, source);
    auto queue = device->CreateQueue(rhi::QueueType::Graphics);
    auto fence = device->CreateFence(0);
    const auto output = std::filesystem::temp_directory_path() / "orbit-terrain-handoff";
    std::filesystem::create_directories(output);
    math::Double3 direction{0.0, 1.0, 0.0};
    f64 height = -1.0e9;
    for (u32 i = 0; i < 128; ++i)
    {
        const f64 y = 1.0 - 2.0 * (static_cast<f64>(i) + 0.5) / 128.0;
        const f64 r = std::sqrt(1.0 - y * y);
        const math::Double3 candidate{r * std::cos(i * 2.39996323), y, r * std::sin(i * 2.39996323)};
        const auto sample = source.Sample({.unitDirection = candidate, .footprintMeters = 1.0});
        if (sample.elevationMeters > height)
        {
            height = sample.elevationMeters;
            direction = candidate;
        }
    }
    const auto frame = world::MakeSurfaceFrame(direction);
    for (u32 shot = 0; shot < 2; ++shot)
    {
        render_view::RenderView view(*device, {.width = 640, .height = 480});
        const f64 altitude = shot == 0 ? 6'000'000.0 : std::max(height, 0.0) + 500.0;
        world::WorldPosition observer{.meters = direction * (planet.radiusMeters + altitude)};
        auto& camera = view.Camera();
        camera.localPositionMeters = observer.meters;
        camera.forward = {static_cast<f32>(-direction.x), static_cast<f32>(-direction.y), static_cast<f32>(-direction.z)};
        camera.up = {static_cast<f32>(frame.north.x), static_cast<f32>(frame.north.y), static_cast<f32>(frame.north.z)};
        camera.nearPlaneMeters = 0.1F;
        camera.farPlaneMeters = 30'000'000.0F;
        std::unique_ptr<terrain_render::TerrainPreviewRenderer> local;
        if (shot == 1)
        {
            terrain_render::TerrainPreviewConfig config;
            config.clipmap = {.levelCount = 9, .gridResolution = 65, .baseSpacingMeters = 1.0};
            config.adaptiveCoverage.enabled = false;
            config.framesInFlight = 1;
            local = std::make_unique<terrain_render::TerrainPreviewRenderer>(
                *device, compiler, planet, fields, observer, config);
        }
        auto allocator = device->CreateCommandAllocator(rhi::QueueType::Graphics);
        auto commands = device->CreateCommandList(*allocator);
        allocator->Reset();
        commands->Reset(*allocator);
        std::array<rhi::Texture*, 4> targets{&view.Color(), &view.SurfaceBaseRoughness(),
            &view.SurfaceNormalMetallic(), &view.SurfaceEmissionClass()};
        for (auto* texture : targets)
        {
            commands->Transition(*texture, rhi::ResourceState::ShaderResource, rhi::ResourceState::RenderTarget);
            commands->ClearColorTarget(*texture, {0.0F, 0.0F, 0.0F, 0.0F});
        }
        commands->ClearDepthTarget(view.Depth(), 0.0F);
        commands->SetRenderTargets(targets, &view.Depth());
        if (local)
        {
            local->Draw(*commands, 0, 640, 480, {
                .forward = {0.0F, -1.0F, 0.0F}, .up = {0.0F, 0.0F, 1.0F},
                .verticalFovRadians = camera.verticalFovRadians,
                .nearPlaneMeters = camera.nearPlaneMeters, .farPlaneMeters = camera.farPlaneMeters});
        }
        globeRenderer.DrawSurface(*commands, view.Color(), view.SurfaceBaseRoughness(),
            view.SurfaceNormalMetallic(), view.SurfaceEmissionClass(), 640, 480, globe,
            camera, 1.0F, {}, &view.Depth());
        for (auto* texture : targets)
            commands->Transition(*texture, rhi::ResourceState::RenderTarget, rhi::ResourceState::ShaderResource);
        commands->Close();
        queue->Submit(*commands);
        queue->Signal(*fence, shot + 1);
        fence->Wait(shot + 1);
        const auto capture = render_view::CaptureFloatBuffer(*device, *queue, view,
            render_view::CaptureBuffer::SceneColor, output / (shot == 0 ? "orbit.ofb" : "ground.ofb"));
        std::ifstream file(capture.path, std::ios::binary);
        file.seekg(16);
        std::vector<f32> pixels(640U * 480U * 4U);
        file.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size() * sizeof(f32)));
        if (!file) return 1;
        // The center of the globe and the downward ground view must have no
        // uncovered pixels, including at local ring boundaries.
        const u32 marginX = shot == 0 ? 250U : 10U;
        const u32 marginY = shot == 0 ? 180U : 10U;
        for (u32 y = marginY; y < 480U - marginY; ++y)
            for (u32 x = marginX; x < 640U - marginX; ++x)
            {
                const auto i = (y * 640U + x) * 4U;
                if (!std::isfinite(pixels[i]) || pixels[i + 3U] < 0.99F)
                {
                    std::cerr << "Uncovered surface in shot " << shot << " at " << x << ',' << y << '\n';
                    return 2;
                }
            }
        std::cout << capture.path << '\n';
    }
}
