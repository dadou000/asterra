#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/terrain_view/ClipmapLayout.hpp>
#include <orbit/world/Planet.hpp>
#include <orbit/world/WorldPosition.hpp>

#include <memory>

namespace orbit::terrain_render
{
struct TerrainPreviewConfig
{
    terrain_view::ClipmapConfig clipmap{
        .levelCount = 7,
        .gridResolution = 65,
        .baseSpacingMeters = 20.0,
        .levelScale = 2.0,
        .overlapCells = 6
    };

    f32 verticalFovRadians{1.22173048F};
    f32 nearPlaneMeters{10.0F};
    f32 farPlaneMeters{250'000.0F};
    bool wireframe{false};
};

class TerrainPreviewRenderer
{
public:
    TerrainPreviewRenderer(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        const world::PlanetDefinition& planet,
        const terrain::TerrainSource& terrainSource,
        const world::WorldPosition& observer,
        TerrainPreviewConfig config = {});

    ~TerrainPreviewRenderer();

    TerrainPreviewRenderer(
        const TerrainPreviewRenderer&) = delete;
    TerrainPreviewRenderer& operator=(
        const TerrainPreviewRenderer&) = delete;
    TerrainPreviewRenderer(
        TerrainPreviewRenderer&&) noexcept;
    TerrainPreviewRenderer& operator=(
        TerrainPreviewRenderer&&) noexcept;

    void Draw(
        rhi::CommandList& commandList,
        u32 targetWidth,
        u32 targetHeight);

    [[nodiscard]] u32 VertexCount() const noexcept;
    [[nodiscard]] u32 IndexCount() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::terrain_render
