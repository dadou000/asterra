#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

#include <memory>

namespace orbit::rhi
{
class CommandList;
class Device;
class Queue;
class Texture;
}

namespace orbit::shader
{
class Compiler;
}

namespace orbit::terrain
{
class AnalyticTerrainSource;
}

namespace orbit::map_render
{
enum class MapLayer : u8
{
    Elevation,
    Tectonics,
    Biomes,
    Temperature,
    Precipitation
};

constexpr u32 kMapLayerCount = 5;

// Inverse of the equirectangular projection PlanetMapRenderer builds its
// layer textures with: normalized UV (0..1 in each axis, origin top-left,
// matching the texture's own row/column order) -> unit direction on the
// planet. Used for click-to-teleport; a free function (not a method)
// since it's a fixed mapping with no renderer state involved.
[[nodiscard]] math::Double3 EquirectDirectionFromUv(
    math::Double2 uv) noexcept;

// Full-screen planet map: generates one flat-colored equirectangular
// texture per MapLayer on a background thread (the planet is static, so
// this only ever happens once), uploads them to the GPU once, and draws
// whichever layer is currently active as a full-screen textured quad.
// Switching layers is then just rebinding an already-uploaded texture.
class PlanetMapRenderer
{
public:
    PlanetMapRenderer(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        rhi::Queue& graphicsQueue,
        std::shared_ptr<const terrain::AnalyticTerrainSource> terrain);
    ~PlanetMapRenderer();

    PlanetMapRenderer(const PlanetMapRenderer&) = delete;
    PlanetMapRenderer& operator=(const PlanetMapRenderer&) = delete;

    // Call once per frame regardless of whether the map is currently
    // visible: advances the background CPU generation and, once it
    // completes, performs the one-time GPU upload. Cheap no-op after
    // that. Must be called before Draw() -- Draw() is a no-op until
    // Ready().
    void Poll();

    [[nodiscard]] bool Ready() const noexcept;

    void SetActiveLayer(MapLayer layer) noexcept;
    [[nodiscard]] MapLayer ActiveLayer() const noexcept;
    void CycleLayer(bool forward) noexcept;

    void Draw(
        rhi::CommandList& commandList,
        rhi::Texture& target,
        u32 targetWidth,
        u32 targetHeight);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::map_render
