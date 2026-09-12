#include <orbit/terrain_hydrology/HydrologyGrid.hpp>

#include <orbit/math/Vector.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <queue>
#include <stdexcept>
#include <vector>

namespace orbit::terrain_hydrology
{
namespace
{
struct NeighborOffset
{
    i8 dx{0};
    i8 dy{0};
    f64 distanceScale{1.0};
};

constexpr std::array<NeighborOffset, 8>
    kNeighbors{{
        {-1, -1, 1.4142135623730951},
        {0, -1, 1.0},
        {1, -1, 1.4142135623730951},
        {-1, 0, 1.0},
        {1, 0, 1.0},
        {-1, 1, 1.4142135623730951},
        {0, 1, 1.0},
        {1, 1, 1.4142135623730951}
    }};

struct FloodNode
{
    u32 index{0};
    f64 elevationMeters{0.0};
};

struct FloodNodeGreater
{
    [[nodiscard]] bool operator()(
        const FloodNode& a,
        const FloodNode& b) const noexcept
    {
        return
            a.elevationMeters >
            b.elevationMeters;
    }
};

[[nodiscard]] std::size_t CellIndex(
    const u32 resolution,
    const u32 x,
    const u32 y) noexcept
{
    return
        static_cast<std::size_t>(y) *
            resolution +
        x;
}

[[nodiscard]] bool IsInside(
    const i32 coordinate,
    const u32 resolution) noexcept
{
    return
        coordinate >= 0 &&
        coordinate <
            static_cast<i32>(
                resolution);
}

[[nodiscard]] bool IsBoundary(
    const u32 x,
    const u32 y,
    const u32 resolution) noexcept
{
    return
        x == 0 ||
        y == 0 ||
        x + 1U == resolution ||
        y + 1U == resolution;
}

void ValidateGrid(
    const HydrologyGrid& grid,
    const char* message)
{
    const u32 resolution =
        grid.config.resolution;

    if (resolution < 3 ||
        grid.cells.size() !=
            static_cast<std::size_t>(
                resolution) *
            resolution ||
        grid.spacingMeters <= 0.0)
    {
        throw std::invalid_argument(
            message);
    }
}
} // namespace

HydrologyCell& HydrologyGrid::At(
    const u32 x,
    const u32 y)
{
    if (x >= config.resolution ||
        y >= config.resolution)
    {
        throw std::out_of_range(
            "Orbit hydrology grid coordinate is out of range.");
    }

    return cells[
        CellIndex(
            config.resolution,
            x,
            y)];
}

const HydrologyCell& HydrologyGrid::At(
    const u32 x,
    const u32 y) const
{
    if (x >= config.resolution ||
        y >= config.resolution)
    {
        throw std::out_of_range(
            "Orbit hydrology grid coordinate is out of range.");
    }

    return cells[
        CellIndex(
            config.resolution,
            x,
            y)];
}

f64 HydrologyGrid::DrainageAreaSquareMeters(
    const u32 x,
    const u32 y) const noexcept
{
    if (x >= config.resolution ||
        y >= config.resolution ||
        spacingMeters <= 0.0)
    {
        return 0.0;
    }

    const f64 cellArea =
        spacingMeters *
        spacingMeters;

    return
        static_cast<f64>(
            cells[
                CellIndex(
                    config.resolution,
                    x,
                    y)].
                flowAccumulation) *
        cellArea;
}

HydrologyGrid BuildHydrologyGrid(
    const world::PlanetDefinition& planet,
    const terrain::TerrainSource& source,
    const world::SurfaceFrame& surfaceFrame,
    HydrologyGridConfig config)
{
    if (planet.radiusMeters <= 0.0)
    {
        throw std::invalid_argument(
            "Orbit hydrology requires a positive planet radius.");
    }

    if (config.resolution < 3)
    {
        throw std::invalid_argument(
            "Orbit hydrology grids require at least three samples per axis.");
    }

    if (config.halfExtentMeters <= 0.0)
    {
        throw std::invalid_argument(
            "Orbit hydrology region extent must be positive.");
    }

    if (config.minimumDrainageDropMeters < 0.0)
    {
        throw std::invalid_argument(
            "Orbit hydrology minimum drainage drop cannot be negative.");
    }

    HydrologyGrid grid{};
    grid.config = config;
    grid.surfaceFrame = surfaceFrame;

    grid.spacingMeters =
        config.halfExtentMeters *
        2.0 /
        static_cast<f64>(
            config.resolution - 1U);

    const f64 footprint =
        config.footprintMeters > 0.0
            ? config.footprintMeters
            : grid.spacingMeters;

    const std::size_t sampleCount =
        static_cast<std::size_t>(
            config.resolution) *
        config.resolution;

    grid.cells.resize(
        sampleCount);

    const f64 halfCells =
        static_cast<f64>(
            config.resolution - 1U) *
        0.5;

    for (u32 y = 0;
         y < config.resolution;
         ++y)
    {
        for (u32 x = 0;
             x < config.resolution;
             ++x)
        {
            const math::Double2 offset{
                (static_cast<f64>(x) -
                 halfCells) *
                    grid.spacingMeters,
                (static_cast<f64>(y) -
                 halfCells) *
                    grid.spacingMeters
            };

            const math::Double3 direction =
                world::DirectionAtSurfaceOffset(
                    planet,
                    surfaceFrame,
                    offset);

            const terrain::TerrainSample sample =
                source.Sample({
                    .unitDirection =
                        direction,
                    .footprintMeters =
                        footprint
                });

            HydrologyCell& cell =
                grid.cells[
                    CellIndex(
                        config.resolution,
                        x,
                        y)];

            const f64 elevation =
                config.useCoarseElevation
                    ? sample.
                        coarseElevationMeters
                    : sample.elevationMeters;

            cell.elevationMeters =
                static_cast<f32>(
                    elevation);

            cell.drainageElevationMeters =
                cell.elevationMeters;

            cell.oceanWeight =
                std::clamp(
                    sample.biomes.ocean,
                    0.0F,
                    1.0F);

            const f32 landWeight =
                1.0F -
                cell.oceanWeight;

            cell.runoffWeight =
                landWeight *
                std::max(
                    sample.climate.
                        precipitation,
                    0.05F);
        }
    }

    if (config.conditionDepressions)
    {
        ConditionDepressions(
            grid);
    }

    RouteHydrology(grid);
    return grid;
}

void ConditionDepressions(
    HydrologyGrid& grid)
{
    ValidateGrid(
        grid,
        "Orbit depression conditioning requires a valid populated hydrology grid.");

    const u32 resolution =
        grid.config.resolution;

    std::vector<u8> visited(
        grid.cells.size(),
        0U);

    std::priority_queue<
        FloodNode,
        std::vector<FloodNode>,
        FloodNodeGreater>
        frontier;

    const auto seed =
        [&grid,
         &visited,
         &frontier](
            const u32 index)
        {
            if (visited[index] != 0U)
            {
                return;
            }

            visited[index] = 1U;

            HydrologyCell& cell =
                grid.cells[index];

            cell.drainageElevationMeters =
                cell.elevationMeters;

            cell.depressionFillMeters =
                0.0F;

            frontier.push({
                .index = index,
                .elevationMeters =
                    static_cast<f64>(
                        cell.
                            drainageElevationMeters)
            });
        };

    for (u32 y = 0;
         y < resolution;
         ++y)
    {
        for (u32 x = 0;
             x < resolution;
             ++x)
        {
            const u32 index =
                static_cast<u32>(
                    CellIndex(
                        resolution,
                        x,
                        y));

            const HydrologyCell& cell =
                grid.cells[index];

            if (IsBoundary(
                    x,
                    y,
                    resolution) ||
                cell.oceanWeight >=
                    0.5F)
            {
                seed(index);
            }
        }
    }

    while (!frontier.empty())
    {
        const FloodNode current =
            frontier.top();

        frontier.pop();

        const u32 currentX =
            current.index %
            resolution;

        const u32 currentY =
            current.index /
            resolution;

        for (const NeighborOffset& neighbor :
             kNeighbors)
        {
            const i32 nx =
                static_cast<i32>(
                    currentX) +
                neighbor.dx;

            const i32 ny =
                static_cast<i32>(
                    currentY) +
                neighbor.dy;

            if (!IsInside(
                    nx,
                    resolution) ||
                !IsInside(
                    ny,
                    resolution))
            {
                continue;
            }

            const u32 neighborIndex =
                static_cast<u32>(
                    CellIndex(
                        resolution,
                        static_cast<u32>(nx),
                        static_cast<u32>(ny)));

            if (visited[
                    neighborIndex] != 0U)
            {
                continue;
            }

            visited[
                neighborIndex] = 1U;

            HydrologyCell& target =
                grid.cells[
                    neighborIndex];

            const f64 minimumTargetElevation =
                current.elevationMeters +
                grid.config.
                    minimumDrainageDropMeters *
                neighbor.distanceScale;

            const f64 conditionedElevation =
                std::max(
                    static_cast<f64>(
                        target.elevationMeters),
                    minimumTargetElevation);

            target.drainageElevationMeters =
                static_cast<f32>(
                    conditionedElevation);

            target.depressionFillMeters =
                static_cast<f32>(
                    std::max(
                        conditionedElevation -
                        static_cast<f64>(
                            target.elevationMeters),
                        0.0));

            frontier.push({
                .index = neighborIndex,
                .elevationMeters =
                    conditionedElevation
            });
        }
    }
}

void RouteHydrology(
    HydrologyGrid& grid)
{
    ValidateGrid(
        grid,
        "Orbit hydrology routing requires a valid populated grid.");

    const u32 resolution =
        grid.config.resolution;

    for (HydrologyCell& cell :
         grid.cells)
    {
        cell.flowDx = 0;
        cell.flowDy = 0;

        cell.flowAccumulation =
            std::max(
                cell.runoffWeight,
                0.0F);
    }

    for (u32 y = 0;
         y < resolution;
         ++y)
    {
        for (u32 x = 0;
             x < resolution;
             ++x)
        {
            HydrologyCell& cell =
                grid.At(
                    x,
                    y);

            if (cell.oceanWeight >=
                0.5F)
            {
                continue;
            }

            f64 bestSlope = 0.0;
            i8 bestDx = 0;
            i8 bestDy = 0;

            for (const NeighborOffset& neighbor :
                 kNeighbors)
            {
                const i32 nx =
                    static_cast<i32>(x) +
                    neighbor.dx;

                const i32 ny =
                    static_cast<i32>(y) +
                    neighbor.dy;

                if (!IsInside(
                        nx,
                        resolution) ||
                    !IsInside(
                        ny,
                        resolution))
                {
                    continue;
                }

                const HydrologyCell& target =
                    grid.At(
                        static_cast<u32>(nx),
                        static_cast<u32>(ny));

                const f64 drop =
                    static_cast<f64>(
                        cell.
                            drainageElevationMeters) -
                    static_cast<f64>(
                        target.
                            drainageElevationMeters);

                if (drop <= 0.0)
                {
                    continue;
                }

                const f64 slope =
                    drop /
                    (grid.spacingMeters *
                     neighbor.distanceScale);

                if (slope >
                    bestSlope)
                {
                    bestSlope = slope;
                    bestDx = neighbor.dx;
                    bestDy = neighbor.dy;
                }
            }

            cell.flowDx = bestDx;
            cell.flowDy = bestDy;
        }
    }

    std::vector<u32> order(
        grid.cells.size());

    std::iota(
        order.begin(),
        order.end(),
        0U);

    std::stable_sort(
        order.begin(),
        order.end(),
        [&grid](
            const u32 a,
            const u32 b)
        {
            return
                grid.cells[a].
                    drainageElevationMeters >
                grid.cells[b].
                    drainageElevationMeters;
        });

    for (const u32 index :
         order)
    {
        const u32 x =
            index %
            resolution;

        const u32 y =
            index /
            resolution;

        const HydrologyCell& cell =
            grid.cells[index];

        if (cell.flowDx == 0 &&
            cell.flowDy == 0)
        {
            continue;
        }

        const i32 nx =
            static_cast<i32>(x) +
            cell.flowDx;

        const i32 ny =
            static_cast<i32>(y) +
            cell.flowDy;

        if (!IsInside(
                nx,
                resolution) ||
            !IsInside(
                ny,
                resolution))
        {
            continue;
        }

        HydrologyCell& target =
            grid.At(
                static_cast<u32>(nx),
                static_cast<u32>(ny));

        target.flowAccumulation +=
            cell.flowAccumulation;
    }
}

std::vector<u32> ExtractRiverCells(
    const HydrologyGrid& grid,
    const f64 minimumDrainageAreaSquareMeters)
{
    std::vector<u32> result;

    if (minimumDrainageAreaSquareMeters <=
        0.0 ||
        grid.spacingMeters <= 0.0)
    {
        return result;
    }

    const f64 cellArea =
        grid.spacingMeters *
        grid.spacingMeters;

    for (u32 index = 0;
         index <
            static_cast<u32>(
                grid.cells.size());
         ++index)
    {
        const HydrologyCell& cell =
            grid.cells[index];

        if (cell.oceanWeight >=
            0.5F)
        {
            continue;
        }

        const f64 drainageArea =
            static_cast<f64>(
                cell.flowAccumulation) *
            cellArea;

        if (drainageArea >=
            minimumDrainageAreaSquareMeters)
        {
            result.push_back(index);
        }
    }

    return result;
}
} // namespace orbit::terrain_hydrology
