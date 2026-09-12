#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/water_render/OceanMesh.hpp>
#include <orbit/world/Planet.hpp>
#include <orbit/world/WorldPosition.hpp>

#include <memory>

namespace orbit::water_render
{
struct OceanCamera
{
    math::Float3 forward{0.0F, -0.28F, 1.0F};
    math::Float3 up{0.0F, 1.0F, 0.0F};
};

struct OceanRendererConfig
{
    OceanMeshConfig mesh{};

    f64 seaLevelMeters{0.0};
    f64 minimumRadiusMeters{25.0};
    f64 maximumRadiusMeters{12'000'000.0};
    f64 horizonOverscan{1.08};

    f32 waveAmplitudeScale{1.0F};

    f32 verticalFovRadians{1.22173048F};
    f32 nearPlaneMeters{10.0F};
    f32 farPlaneMeters{12'000'000.0F};
};

struct OceanRenderStats
{
    u32 vertices{0};
    u32 indices{0};
    u32 drawCallsLastFrame{0};
    f64 effectiveRadiusMeters{0.0};
};

class OceanRenderer
{
public:
    OceanRenderer(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        world::PlanetDefinition planet,
        const world::WorldPosition& observer,
        OceanRendererConfig config = {});

    ~OceanRenderer();

    OceanRenderer(
        const OceanRenderer&) = delete;

    OceanRenderer& operator=(
        const OceanRenderer&) = delete;

    void UpdateObserver(
        const world::WorldPosition& observer);

    void Draw(
        rhi::CommandList& commandList,
        rhi::Texture& colorTarget,
        rhi::Texture& depthTarget,
        u32 targetWidth,
        u32 targetHeight,
        const OceanCamera& camera);

    [[nodiscard]] const OceanRenderStats&
    Stats() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::water_render
