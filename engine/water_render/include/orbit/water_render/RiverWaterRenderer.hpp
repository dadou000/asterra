#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/terrain_region/DerivedTerrainRegionCache.hpp>
#include <orbit/world/Planet.hpp>
#include <orbit/world/WorldPosition.hpp>

#include <memory>

namespace orbit::water_render
{
struct RiverWaterCamera
{
    math::Float3 forward{0.0F, -0.28F, 1.0F};
    math::Float3 up{0.0F, 1.0F, 0.0F};
};

struct RiverWaterRendererConfig
{
    u32 framesInFlight{3};
    u32 maximumSegments{16'384};
    u32 maximumLakeCells{8'192};

    f32 verticalFovRadians{1.22173048F};
    f32 nearPlaneMeters{10.0F};
    f32 farPlaneMeters{12'000'000.0F};

    f64 maximumDrawDistanceMeters{180'000.0};
    f64 surfaceOffsetMeters{0.08};
};

struct RiverWaterRenderStats
{
    u32 readyRegionsLastFrame{0};
    u32 visibleSegmentsLastFrame{0};
    u32 visibleLakeCellsLastFrame{0};

    u32 truncatedSegmentsLastFrame{0};
    u32 truncatedLakeCellsLastFrame{0};

    u64 uploadedBytesLastFrame{0};
    u32 drawCallsLastFrame{0};
};

class RiverWaterRenderer
{
public:
    // `fineRegionCache`, when provided, is a second region cache at a
    // finer tile level. Wherever it has ready coverage for a given
    // segment or lake cell, that finer version is drawn instead of
    // the coarse one, instead of drawing both (which would either
    // z-fight or double up water surfaces at slightly different
    // elevations).
    RiverWaterRenderer(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        world::PlanetDefinition planet,
        std::shared_ptr<
            terrain_region::DerivedTerrainRegionCache> regionCache,
        std::shared_ptr<
            terrain_region::DerivedTerrainRegionCache> fineRegionCache,
        const world::WorldPosition& observer,
        RiverWaterRendererConfig config = {});

    RiverWaterRenderer(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        world::PlanetDefinition planet,
        std::shared_ptr<
            terrain_region::DerivedTerrainRegionCache> regionCache,
        const world::WorldPosition& observer,
        RiverWaterRendererConfig config = {});

    ~RiverWaterRenderer();

    RiverWaterRenderer(
        const RiverWaterRenderer&) = delete;

    RiverWaterRenderer& operator=(
        const RiverWaterRenderer&) = delete;

    void UpdateObserver(
        const world::WorldPosition& observer);

    void Draw(
        rhi::CommandList& commandList,
        rhi::Texture& colorTarget,
        rhi::Texture& depthTarget,
        u32 frameIndex,
        u32 targetWidth,
        u32 targetHeight,
        const RiverWaterCamera& camera);

    [[nodiscard]] const RiverWaterRenderStats&
    Stats() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::water_render
