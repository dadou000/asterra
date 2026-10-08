#include <orbit/terrain_bake/RiverBaker.hpp>

#include <orbit/terrain/BakedTectonics.hpp>
#include <orbit/terrain/TerrainContracts.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <numbers>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

namespace orbit::terrain_bake
{
namespace
{
constexpr u64 kRiverBakeAlgorithmVersion = 1;
constexpr f64 kSecondsPerYear = 31'557'600.0;
// Strictly positive slope across flats after depression filling.
constexpr f64 kFillEpsilonMeters = 1.0e-4;
constexpr u32 kNeighborCount = 8;
// A planet needs at least this share of ocean cells for rivers to have
// somewhere to go.
constexpr f64 kMinimumOceanShare = 0.01;

struct CellEntry
{
    f64 elevation;
    u32 cell;
    [[nodiscard]] bool operator>(const CellEntry& other) const noexcept
    {
        return elevation != other.elevation ? elevation > other.elevation
                                            : cell > other.cell;
    }
};

[[nodiscard]] f64 ChannelWidthMeters(const f64 discharge) noexcept
{
    return 3.5 * std::sqrt(discharge);
}

[[nodiscard]] f64 ChannelDepthMeters(const f64 discharge) noexcept
{
    return 0.4 * std::cbrt(discharge);
}
} // namespace

u64 RiverBakeRecipeHash(
    const world::PlanetDefinition& planet,
    const terrain::AnalyticTerrainDesc& desc,
    const RiverBakeOptions& options)
{
    u64 hash = terrain::StableCombine64(
        0x52495645524B4531ULL, kRiverBakeAlgorithmVersion);
    hash = terrain::StableCombine64(hash, terrain::TerrainRecipeHash(planet, desc));
    hash = terrain::StableCombine64(hash, options.resolution);
    hash = terrain::StableCombine64(
        hash, std::bit_cast<u64>(options.minimumDischargeCubicMetersPerSecond));
    hash = terrain::StableCombine64(hash, std::bit_cast<u64>(options.annualRunoffMeters));
    return hash == 0U ? 1U : hash;
}

std::shared_ptr<const terrain::BakedRiverNetwork> BakeRivers(
    const world::PlanetDefinition& planet,
    terrain::AnalyticTerrainDesc desc,
    const RiverBakeOptions& options,
    BakeControl* const control)
{
    if (!options.IsValid())
    {
        throw std::invalid_argument(
            "River bake options are invalid (resolution 16-1024, positive discharge threshold).");
    }

    desc.bakedRivers.reset();
    const u64 recipeHash = RiverBakeRecipeHash(planet, desc, options);
    const terrain::AnalyticTerrainSource source(planet, desc);
    const f64 seaLevel = desc.global.seaLevelMeters;

    const u32 resolution = options.resolution;
    const std::size_t cells =
        static_cast<std::size_t>(terrain::kBakedTectonicFaces) * resolution * resolution;
    const f64 cellWidthMeters =
        planet.radiusMeters * (std::numbers::pi * 0.5) / static_cast<f64>(resolution);

    std::vector<math::Double3> direction(cells);
    std::vector<f32> elevation(cells);
    std::vector<f32> precipitation(cells);
    std::vector<f64> areaSquareMeters(cells);
    std::vector<i32> neighbors(cells * kNeighborCount, -1);

    const u32 rowsTotal = terrain::kBakedTectonicFaces * resolution;
    if (control != nullptr)
    {
        control->rowsTotal.store(rowsTotal + 4U, std::memory_order_relaxed);
        control->rowsDone.store(0U, std::memory_order_relaxed);
    }
    const auto cancelled = [control]() noexcept
    {
        return control != nullptr && control->cancel.load(std::memory_order_relaxed);
    };

    // 1. Sample the terrain the river will be cut into, and build the 8-way
    //    neighbour table (across cube-face edges too).
    std::atomic<u32> nextRow{0U};
    const auto sampleRows = [&]()
    {
        constexpr i32 kOffsets[kNeighborCount][2] = {
            {-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
        for (;;)
        {
            if (cancelled())
            {
                return;
            }
            const u32 row = nextRow.fetch_add(1U, std::memory_order_relaxed);
            if (row >= rowsTotal)
            {
                return;
            }
            const u32 face = row / resolution;
            const u32 y = row % resolution;
            for (u32 x = 0; x < resolution; ++x)
            {
                const std::size_t cell =
                    (static_cast<std::size_t>(face) * resolution + y) * resolution + x;
                const math::Double3 d = terrain::BakedTectonicTexelDirection(
                    face, static_cast<i32>(x), static_cast<i32>(y), resolution);
                direction[cell] = d;

                const terrain::TerrainSample sample = source.Sample({
                    .unitDirection = d,
                    .footprintMeters = cellWidthMeters,
                    .planet = planet.id,
                    .radialOffsetMeters = 0.0});
                elevation[cell] = static_cast<f32>(sample.elevationMeters);
                precipitation[cell] = std::clamp(sample.climate.precipitation, 0.0F, 1.0F);

                // Gnomonic cell solid angle: (2/R)^2 / (1 + u^2 + v^2)^(3/2).
                const f64 u = (static_cast<f64>(x) + 0.5) / resolution * 2.0 - 1.0;
                const f64 v = (static_cast<f64>(y) + 0.5) / resolution * 2.0 - 1.0;
                const f64 solidAngle = (4.0 / (static_cast<f64>(resolution) * resolution)) /
                    std::pow(1.0 + u * u + v * v, 1.5);
                areaSquareMeters[cell] = solidAngle * planet.radiusMeters * planet.radiusMeters;

                for (u32 n = 0; n < kNeighborCount; ++n)
                {
                    const math::Double3 nd = terrain::BakedTectonicTexelDirection(
                        face, static_cast<i32>(x) + kOffsets[n][0],
                        static_cast<i32>(y) + kOffsets[n][1], resolution);
                    const world::CubeCoordinate cube = world::UnitDirectionToCube(nd);
                    const f64 fres = static_cast<f64>(resolution);
                    const u32 nx = static_cast<u32>(std::clamp(
                        std::floor((cube.uv.x + 1.0) * 0.5 * fres), 0.0, fres - 1.0));
                    const u32 ny = static_cast<u32>(std::clamp(
                        std::floor((cube.uv.y + 1.0) * 0.5 * fres), 0.0, fres - 1.0));
                    const std::size_t ncell =
                        (static_cast<std::size_t>(cube.face) * resolution + ny) * resolution + nx;
                    neighbors[cell * kNeighborCount + n] =
                        ncell == cell ? -1 : static_cast<i32>(ncell);
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
    {
        std::vector<std::thread> threads;
        for (u32 i = 1U; i < workers; ++i)
        {
            threads.emplace_back(sampleRows);
        }
        sampleRows();
        for (auto& thread : threads)
        {
            thread.join();
        }
    }
    if (cancelled())
    {
        return nullptr;
    }

    // 2. Priority-flood from the ocean: every land cell drains to the cell it
    //    was reached from, so depressions fill and every river reaches the sea.
    std::vector<u8> ocean(cells, 0U);
    std::size_t oceanCells = 0;
    for (std::size_t c = 0; c < cells; ++c)
    {
        if (static_cast<f64>(elevation[c]) <= seaLevel)
        {
            ocean[c] = 1U;
            ++oceanCells;
        }
    }

    const auto empty = [&]()
    {
        return std::make_shared<const terrain::BakedRiverNetwork>(
            terrain::BakedRiverNetwork::Build(planet.radiusMeters, recipeHash, {}, {}));
    };
    if (static_cast<f64>(oceanCells) < kMinimumOceanShare * static_cast<f64>(cells))
    {
        return empty();
    }

    std::vector<f64> filled(cells, 0.0);
    std::vector<i32> parent(cells, -1);
    std::vector<u8> closed(cells, 0U);
    std::vector<u32> order;
    order.reserve(cells);
    std::priority_queue<CellEntry, std::vector<CellEntry>, std::greater<CellEntry>> heap;
    for (std::size_t c = 0; c < cells; ++c)
    {
        if (ocean[c] != 0U)
        {
            filled[c] = static_cast<f64>(elevation[c]);
            closed[c] = 1U;
            heap.push({filled[c], static_cast<u32>(c)});
        }
    }
    while (!heap.empty())
    {
        const CellEntry current = heap.top();
        heap.pop();
        order.push_back(current.cell);
        for (u32 n = 0; n < kNeighborCount; ++n)
        {
            const i32 neighbor = neighbors[static_cast<std::size_t>(current.cell) * kNeighborCount + n];
            if (neighbor < 0 || closed[static_cast<std::size_t>(neighbor)] != 0U)
            {
                continue;
            }
            const std::size_t nc = static_cast<std::size_t>(neighbor);
            closed[nc] = 1U;
            parent[nc] = static_cast<i32>(current.cell);
            filled[nc] = std::max(
                static_cast<f64>(elevation[nc]), current.elevation + kFillEpsilonMeters);
            heap.push({filled[nc], static_cast<u32>(nc)});
        }
    }
    if (control != nullptr)
    {
        control->rowsDone.fetch_add(1U, std::memory_order_relaxed);
    }
    if (cancelled())
    {
        return nullptr;
    }

    // 3. Accumulate discharge down the drainage tree. A child always follows
    //    its parent in the flood order, so the reverse order visits children
    //    first.
    std::vector<f64> discharge(cells, 0.0);
    for (std::size_t c = 0; c < cells; ++c)
    {
        if (ocean[c] == 0U)
        {
            discharge[c] = static_cast<f64>(precipitation[c]) *
                options.annualRunoffMeters * areaSquareMeters[c] / kSecondsPerYear;
        }
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it)
    {
        const std::size_t c = *it;
        const i32 p = parent[c];
        if (p >= 0 && ocean[c] == 0U)
        {
            discharge[static_cast<std::size_t>(p)] += discharge[c];
        }
    }
    if (control != nullptr)
    {
        control->rowsDone.fetch_add(1U, std::memory_order_relaxed);
    }

    // 4. Extract the rivers: land cells above the discharge threshold, plus the
    //    ocean cell each river mouth empties into.
    std::vector<terrain::BakedRiverNode> nodes;
    std::vector<terrain::BakedRiverSegment> segments;
    std::vector<i32> nodeOfCell(cells, -1);
    const auto isRiver = [&](const std::size_t c)
    {
        return ocean[c] == 0U && parent[c] >= 0 &&
               discharge[c] >= options.minimumDischargeCubicMetersPerSecond;
    };

    const auto addNode = [&](const std::size_t cell, const f64 q)
    {
        terrain::BakedRiverNode node;
        node.direction = direction[cell];
        node.elevationMeters = elevation[cell];
        node.dischargeCubicMetersPerSecond = static_cast<f32>(q);
        node.widthMeters = static_cast<f32>(ChannelWidthMeters(q));
        node.depthMeters = static_cast<f32>(ChannelDepthMeters(q));
        nodeOfCell[cell] = static_cast<i32>(nodes.size());
        nodes.push_back(node);
    };

    for (std::size_t c = 0; c < cells; ++c)
    {
        if (isRiver(c))
        {
            addNode(c, discharge[c]);
        }
    }
    const std::size_t riverNodeCount = nodes.size();
    for (std::size_t c = 0; c < cells; ++c)
    {
        if (nodeOfCell[c] < 0 || static_cast<std::size_t>(nodeOfCell[c]) >= riverNodeCount)
        {
            continue;
        }
        const std::size_t p = static_cast<std::size_t>(parent[c]);
        if (isRiver(p))
        {
            segments.push_back({static_cast<u32>(nodeOfCell[c]), static_cast<u32>(nodeOfCell[p])});
        }
        else
        {
            // The mouth: continue into the ocean cell so the river meets the sea.
            if (nodeOfCell[p] < 0)
            {
                addNode(p, discharge[p]);
            }
            segments.push_back({static_cast<u32>(nodeOfCell[c]), static_cast<u32>(nodeOfCell[p])});
        }
    }

    // Smooth the grid zig-zag of single-thread stretches (one reach in, one
    // out); confluences and mouths keep their positions.
    std::vector<i32> downstreamOf(nodes.size(), -1);
    std::vector<i32> upstreamOnly(nodes.size(), -1);
    std::vector<u32> upstreamCount(nodes.size(), 0U);
    for (const auto& segment : segments)
    {
        downstreamOf[segment.upstream] = static_cast<i32>(segment.downstream);
        upstreamOnly[segment.downstream] = static_cast<i32>(segment.upstream);
        ++upstreamCount[segment.downstream];
    }
    for (int pass = 0; pass < 3; ++pass)
    {
        std::vector<math::Double3> smoothed(nodes.size());
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            smoothed[i] = nodes[i].direction;
            if (upstreamCount[i] == 1U && downstreamOf[i] >= 0)
            {
                smoothed[i] = math::Normalize(
                    nodes[static_cast<std::size_t>(upstreamOnly[i])].direction * 0.25 +
                    nodes[i].direction * 0.5 +
                    nodes[static_cast<std::size_t>(downstreamOf[i])].direction * 0.25);
            }
        }
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            nodes[i].direction = smoothed[i];
        }
    }

    // Basin = the mouth each node drains to.
    std::vector<i32> basin(nodes.size(), -1);
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        std::vector<std::size_t> path;
        std::size_t at = i;
        while (basin[at] < 0 && downstreamOf[at] >= 0)
        {
            path.push_back(at);
            at = static_cast<std::size_t>(downstreamOf[at]);
        }
        const i32 mouth = basin[at] >= 0 ? basin[at] : static_cast<i32>(at);
        basin[at] = mouth;
        for (const std::size_t visited : path)
        {
            basin[visited] = mouth;
        }
    }
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        nodes[i].basin = static_cast<u32>(basin[i]);
    }

    if (control != nullptr)
    {
        control->rowsDone.fetch_add(2U, std::memory_order_relaxed);
    }
    return std::make_shared<const terrain::BakedRiverNetwork>(
        terrain::BakedRiverNetwork::Build(
            planet.radiusMeters, recipeHash, std::move(nodes), std::move(segments)));
}
} // namespace orbit::terrain_bake
