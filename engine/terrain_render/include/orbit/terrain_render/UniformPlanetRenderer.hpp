#pragma once
#include <orbit/terrain_render/TerrainPreviewRenderer.hpp>
#include <orbit/terrain_stream/UniformPlanetMesh.hpp>
#include <memory>

namespace orbit::terrain_render
{
class UniformPlanetRenderer
{
public:
    UniformPlanetRenderer(rhi::Device& device, const shader::Compiler& compiler,
        world::PlanetDefinition planet, std::shared_ptr<const terrain::TerrainSource> source,
        TerrainPreviewConfig config);
    ~UniformPlanetRenderer();
    // -1 returns to automatic clipmaps. Generation runs off the render thread.
    void RequestLod(i32 lod);
    void Poll();
    [[nodiscard]] bool HasReadyMesh() const;
    // Caller must wait for outstanding graphics work before replacing buffers.
    void CommitReadyMesh();
    void Draw(rhi::CommandList& commands, const world::WorldPosition& observer,
        const world::SurfaceFrame& frame, u32 width, u32 height, const TerrainPreviewCamera& camera);
    [[nodiscard]] i32 RequestedLod() const;
    [[nodiscard]] i32 ActiveLod() const;
    [[nodiscard]] bool Building() const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
