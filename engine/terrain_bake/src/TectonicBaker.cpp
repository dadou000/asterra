#include <orbit/terrain_bake/TectonicBaker.hpp>

#include <algorithm>
#include <stdexcept>
#include <thread>
#include <vector>

namespace orbit::terrain_bake
{
std::shared_ptr<const terrain::BakedTectonicRasters> BakeTectonics(
    const world::PlanetDefinition& planet,
    terrain::AnalyticTerrainDesc desc,
    const TectonicBakeOptions& options,
    BakeControl* const control)
{
    if (!options.IsValid())
    {
        throw std::invalid_argument(
            "Tectonic bake resolution must be between 16 and 2048.");
    }

    // The baker evaluates the plate model, never a previous bake.
    desc.global.bakedTectonics.reset();
    const u64 recipeHash = terrain::TectonicBakeRecipeHash(planet, desc);
    const terrain::AnalyticTerrainSource source(planet, desc);
    const terrain::GlobalTerrainFields& fields = source.GlobalFields();

    const u32 resolution = options.resolution;
    const u32 stride = resolution + 2U;
    const std::size_t gutterTexels =
        static_cast<std::size_t>(terrain::kBakedTectonicFaces) * stride * stride;
    const std::size_t plateTexels =
        static_cast<std::size_t>(terrain::kBakedTectonicFaces) * resolution * resolution;

    std::array<std::vector<f32>, terrain::kBakedTectonicLayerCount> layers;
    for (auto& layer : layers)
    {
        layer.assign(gutterTexels, 0.0F);
    }
    std::vector<u8> plate(plateTexels, 0U);
    std::vector<u8> neighbour(plateTexels, 0U);

    const u32 rowsTotal = terrain::kBakedTectonicFaces * stride;
    if (control != nullptr)
    {
        control->rowsTotal.store(rowsTotal, std::memory_order_relaxed);
        control->rowsDone.store(0U, std::memory_order_relaxed);
    }

    std::atomic<u32> nextRow{0U};
    const auto work = [&]()
    {
        for (;;)
        {
            if (control != nullptr &&
                control->cancel.load(std::memory_order_relaxed))
            {
                return;
            }

            const u32 row = nextRow.fetch_add(1U, std::memory_order_relaxed);
            if (row >= rowsTotal)
            {
                return;
            }

            const u32 face = row / stride;
            const i32 y = static_cast<i32>(row % stride) - 1;
            for (i32 x = -1; x <= static_cast<i32>(resolution); ++x)
            {
                const math::Double3 direction =
                    terrain::BakedTectonicTexelDirection(face, x, y, resolution);
                const terrain::BakedTectonicTexel texel =
                    fields.EvaluateTectonicTexel(direction);

                const std::size_t index =
                    (static_cast<std::size_t>(face) * stride +
                     static_cast<std::size_t>(y + 1)) * stride +
                    static_cast<std::size_t>(x + 1);
                for (std::size_t layer = 0; layer < layers.size(); ++layer)
                {
                    layers[layer][index] = texel.values[layer];
                }

                if (x >= 0 && y >= 0 &&
                    x < static_cast<i32>(resolution) &&
                    y < static_cast<i32>(resolution))
                {
                    const std::size_t plateIndex =
                        (static_cast<std::size_t>(face) * resolution +
                         static_cast<std::size_t>(y)) * resolution +
                        static_cast<std::size_t>(x);
                    plate[plateIndex] = texel.plate;
                    neighbour[plateIndex] = texel.neighbour;
                }
            }

            if (control != nullptr)
            {
                control->rowsDone.fetch_add(1U, std::memory_order_relaxed);
            }
        }
    };

    u32 workers = options.workerThreads;
    if (workers == 0U)
    {
        workers = std::max(1U, std::thread::hardware_concurrency());
    }
    workers = std::min(workers, rowsTotal);

    std::vector<std::thread> threads;
    threads.reserve(workers > 0U ? workers - 1U : 0U);
    for (u32 i = 1U; i < workers; ++i)
    {
        threads.emplace_back(work);
    }
    work();
    for (auto& thread : threads)
    {
        thread.join();
    }

    if (control != nullptr &&
        control->cancel.load(std::memory_order_relaxed))
    {
        return nullptr;
    }

    const u32 plateCount = fields.TectonicPlateCount();
    std::array<u8, terrain::kBakedTectonicMaxPlates> continental{};
    for (u32 i = 0U; i < plateCount && i < terrain::kBakedTectonicMaxPlates; ++i)
    {
        continental[i] = fields.TectonicPlateIsContinental(i) ? 1U : 0U;
    }

    return std::make_shared<const terrain::BakedTectonicRasters>(
        terrain::BakedTectonicRasters::FromFloats(
            resolution,
            recipeHash,
            layers,
            std::move(plate),
            std::move(neighbour),
            continental,
            plateCount));
}
} // namespace orbit::terrain_bake
