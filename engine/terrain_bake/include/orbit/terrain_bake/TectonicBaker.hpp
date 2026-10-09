#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain/BakedTectonics.hpp>
#include <orbit/world/Planet.hpp>

#include <atomic>
#include <memory>

namespace orbit::terrain_bake
{
struct TectonicBakeOptions
{
    // Texels per cube-face edge (each face also carries a one-texel gutter).
    // 256 is about 40 km per texel on an Earth-sized planet, well below the
    // width of any boundary the plate model produces.
    u32 resolution{256};
    // 0 = hardware concurrency.
    u32 workerThreads{0};

    [[nodiscard]] bool IsValid() const noexcept
    {
        return resolution >= 16U && resolution <= 2048U;
    }
};

// Shared between the baking thread and its observers.
struct BakeControl
{
    std::atomic<bool> cancel{false};
    std::atomic<u32> rowsDone{0};
    std::atomic<u32> rowsTotal{0};

    [[nodiscard]] f32 Progress() const noexcept
    {
        const u32 total = rowsTotal.load(std::memory_order_relaxed);
        return total == 0U
            ? 0.0F
            : static_cast<f32>(rowsDone.load(std::memory_order_relaxed)) /
                  static_cast<f32>(total);
    }
};

// Evaluates the plate model once per texel of every cube face (gutter
// included) and quantizes the result. Returns nullptr when cancelled; throws
// std::invalid_argument for invalid options. `desc.global.bakedTectonics` is
// ignored: the baker always evaluates the plate model itself.
[[nodiscard]] std::shared_ptr<const terrain::BakedTectonicRasters>
BakeTectonics(
    const world::PlanetDefinition& planet,
    terrain::AnalyticTerrainDesc desc,
    const TectonicBakeOptions& options,
    BakeControl* control = nullptr);
} // namespace orbit::terrain_bake
