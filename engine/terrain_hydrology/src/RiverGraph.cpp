#include <orbit/terrain_hydrology/RiverGraph.hpp>

#include <limits>
#include <stdexcept>
#include <vector>

namespace orbit::terrain_hydrology
{
namespace
{
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
} // namespace

RiverGraph BuildRiverGraph(
    const HydrologyGrid& grid,
    const f64 minimumDrainageAreaSquareMeters)
{
    RiverGraph graph{};

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
            "Orbit river graph generation requires a valid routed hydrology grid.");
    }

    if (minimumDrainageAreaSquareMeters <=
        0.0)
    {
        return graph;
    }

    constexpr u32 invalidNode =
        std::numeric_limits<u32>::max();

    std::vector<u32> nodeForCell(
        grid.cells.size(),
        invalidNode);

    const f64 halfCells =
        static_cast<f64>(
            resolution - 1U) *
        0.5;

    const auto ensureNode =
        [&graph,
         &grid,
         &nodeForCell,
         halfCells,
         resolution](
            const u32 cellIndex) -> u32
        {
            u32& existing =
                nodeForCell[cellIndex];

            if (existing != invalidNode)
            {
                return existing;
            }

            const u32 x =
                cellIndex %
                resolution;

            const u32 y =
                cellIndex /
                resolution;

            const HydrologyCell& cell =
                grid.cells[cellIndex];

            existing =
                static_cast<u32>(
                    graph.nodes.size());

            graph.nodes.push_back({
                .sourceCellIndex =
                    cellIndex,
                .offsetMeters = {
                    (static_cast<f64>(x) -
                     halfCells) *
                        grid.spacingMeters,
                    (static_cast<f64>(y) -
                     halfCells) *
                        grid.spacingMeters
                },
                .elevationMeters =
                    cell.elevationMeters,
                .drainageElevationMeters =
                    cell.drainageElevationMeters,
                .depressionFillMeters =
                    cell.depressionFillMeters,
                .drainageAreaSquareMeters =
                    static_cast<f64>(
                        cell.
                            flowAccumulation) *
                    grid.spacingMeters *
                    grid.spacingMeters,
                .oceanWeight =
                    cell.oceanWeight
            });

            return existing;
        };

    for (u32 cellIndex = 0;
         cellIndex <
            static_cast<u32>(
                grid.cells.size());
         ++cellIndex)
    {
        const HydrologyCell& cell =
            grid.cells[cellIndex];

        if (cell.oceanWeight >= 0.5F)
        {
            continue;
        }

        const f64 drainageArea =
            static_cast<f64>(
                cell.flowAccumulation) *
            grid.spacingMeters *
            grid.spacingMeters;

        if (drainageArea <
            minimumDrainageAreaSquareMeters)
        {
            continue;
        }

        if (cell.flowDx == 0 &&
            cell.flowDy == 0)
        {
            static_cast<void>(
                ensureNode(
                    cellIndex));

            continue;
        }

        const u32 x =
            cellIndex %
            resolution;

        const u32 y =
            cellIndex /
            resolution;

        const i32 downstreamX =
            static_cast<i32>(x) +
            cell.flowDx;

        const i32 downstreamY =
            static_cast<i32>(y) +
            cell.flowDy;

        if (!IsInside(
                downstreamX,
                resolution) ||
            !IsInside(
                downstreamY,
                resolution))
        {
            continue;
        }

        const u32 downstreamCell =
            static_cast<u32>(
                downstreamY) *
                resolution +
            static_cast<u32>(
                downstreamX);

        const u32 upstreamNode =
            ensureNode(
                cellIndex);

        const u32 downstreamNode =
            ensureNode(
                downstreamCell);

        graph.segments.push_back({
            .upstreamNode =
                upstreamNode,
            .downstreamNode =
                downstreamNode
        });
    }

    return graph;
}
} // namespace orbit::terrain_hydrology
