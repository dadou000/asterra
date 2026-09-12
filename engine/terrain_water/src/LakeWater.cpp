#include <orbit/terrain_water/LakeWater.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>
#include <vector>

namespace orbit::terrain_water
{
namespace
{
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

[[nodiscard]] math::Double2 CellOffset(
    const u32 resolution,
    const f64 spacingMeters,
    const u32 x,
    const u32 y) noexcept
{
    const f64 halfCells =
        static_cast<f64>(
            resolution - 1U) *
        0.5;

    return {
        (static_cast<f64>(x) -
         halfCells) *
            spacingMeters,
        (static_cast<f64>(y) -
         halfCells) *
            spacingMeters
    };
}

[[nodiscard]] bool IsCoreOwned(
    const math::Double2& offset,
    const f64 coreHalfExtentMeters) noexcept
{
    return
        std::abs(offset.x) <=
            coreHalfExtentMeters &&
        std::abs(offset.y) <=
            coreHalfExtentMeters;
}
} // namespace

LakeWaterField BuildLakeWaterField(
    const terrain_hydrology::HydrologyGrid& hydrology,
    const f64 coreHalfExtentMeters,
    const LakeWaterConfig config)
{
    const u32 resolution =
        hydrology.config.resolution;

    if (resolution < 3 ||
        hydrology.cells.size() !=
            static_cast<std::size_t>(
                resolution) *
            resolution ||
        hydrology.spacingMeters <= 0.0 ||
        !std::isfinite(
            coreHalfExtentMeters) ||
        coreHalfExtentMeters <= 0.0 ||
        !std::isfinite(
            config.minimumWaterDepthMeters) ||
        config.minimumWaterDepthMeters <= 0.0 ||
        config.minimumCellsPerBasin == 0)
    {
        throw std::invalid_argument(
            "Orbit lake water requires a valid hydrology grid and configuration.");
    }

    LakeWaterField result{};

    result.surfaceFrame =
        hydrology.surfaceFrame;

    result.cellSpacingMeters =
        hydrology.spacingMeters;

    result.coreHalfExtentMeters =
        coreHalfExtentMeters;

    std::vector<u8> visited(
        hydrology.cells.size(),
        0U);

    std::vector<u32> component;
    component.reserve(
        hydrology.cells.size());

    constexpr i32 dx[4]{
        -1,
        1,
        0,
        0
    };

    constexpr i32 dy[4]{
        0,
        0,
        -1,
        1
    };

    for (u32 seedIndex = 0;
         seedIndex <
            static_cast<u32>(
                hydrology.cells.size());
         ++seedIndex)
    {
        if (visited[seedIndex] != 0U)
        {
            continue;
        }

        const auto& seed =
            hydrology.cells[
                seedIndex];

        if (seed.oceanWeight >= 0.5F ||
            static_cast<f64>(
                seed.depressionFillMeters) <
                config.minimumWaterDepthMeters)
        {
            visited[seedIndex] = 1U;
            continue;
        }

        component.clear();

        std::queue<u32> frontier;
        frontier.push(seedIndex);
        visited[seedIndex] = 1U;

        f64 basinSurface =
            std::numeric_limits<f64>::max();

        while (!frontier.empty())
        {
            const u32 index =
                frontier.front();

            frontier.pop();

            component.push_back(index);

            const auto& cell =
                hydrology.cells[index];

            basinSurface =
                std::min(
                    basinSurface,
                    static_cast<f64>(
                        cell.
                            drainageElevationMeters));

            const u32 x =
                index %
                resolution;

            const u32 y =
                index /
                resolution;

            for (u32 neighbor = 0;
                 neighbor < 4;
                 ++neighbor)
            {
                const i32 nx =
                    static_cast<i32>(x) +
                    dx[neighbor];

                const i32 ny =
                    static_cast<i32>(y) +
                    dy[neighbor];

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
                            static_cast<u32>(
                                nx),
                            static_cast<u32>(
                                ny)));

                if (visited[
                        neighborIndex] != 0U)
                {
                    continue;
                }

                const auto& target =
                    hydrology.cells[
                        neighborIndex];

                if (target.oceanWeight >= 0.5F ||
                    static_cast<f64>(
                        target.
                            depressionFillMeters) <
                        config.
                            minimumWaterDepthMeters)
                {
                    continue;
                }

                visited[
                    neighborIndex] = 1U;

                frontier.push(
                    neighborIndex);
            }
        }

        if (component.size() <
            config.minimumCellsPerBasin)
        {
            continue;
        }

        std::vector<LakeWaterCell>
            ownedCells;

        ownedCells.reserve(
            component.size());

        f64 maximumDepth = 0.0;

        for (const u32 index :
             component)
        {
            const u32 x =
                index %
                resolution;

            const u32 y =
                index /
                resolution;

            const math::Double2 offset =
                CellOffset(
                    resolution,
                    hydrology.spacingMeters,
                    x,
                    y);

            if (!IsCoreOwned(
                    offset,
                    coreHalfExtentMeters))
            {
                continue;
            }

            const auto& cell =
                hydrology.cells[
                    index];

            const f64 terrainElevation =
                static_cast<f64>(
                    cell.elevationMeters);

            const f64 depth =
                basinSurface -
                terrainElevation;

            if (depth <
                config.minimumWaterDepthMeters)
            {
                continue;
            }

            maximumDepth =
                std::max(
                    maximumDepth,
                    depth);

            ownedCells.push_back({
                .sourceCellIndex =
                    index,
                .offsetMeters =
                    offset,
                .terrainElevationMeters =
                    cell.elevationMeters,
                .surfaceElevationMeters =
                    static_cast<f32>(
                        basinSurface),
                .depthMeters =
                    static_cast<f32>(
                        depth),
                .basinIndex = 0
            });
        }

        if (ownedCells.empty())
        {
            continue;
        }

        const u32 basinIndex =
            static_cast<u32>(
                result.basins.size());

        const u32 firstCell =
            static_cast<u32>(
                result.cells.size());

        for (LakeWaterCell& cell :
             ownedCells)
        {
            cell.basinIndex =
                basinIndex;

            result.cells.push_back(
                cell);
        }

        result.basins.push_back({
            .surfaceElevationMeters =
                static_cast<f32>(
                    basinSurface),
            .maximumDepthMeters =
                static_cast<f32>(
                    maximumDepth),
            .areaSquareMeters =
                static_cast<f64>(
                    ownedCells.size()) *
                hydrology.spacingMeters *
                hydrology.spacingMeters,
            .firstCell =
                firstCell,
            .cellCount =
                static_cast<u32>(
                    ownedCells.size())
        });
    }

    return result;
}
} // namespace orbit::terrain_water
