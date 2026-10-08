#include <orbit/terrain_water/LakeWater.hpp>
#include <orbit/terrain/TerrainContracts.hpp>

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
        !std::isfinite(hydrology.spacingMeters) ||
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

    result.resolution = resolution;
    result.bedElevationsMeters.reserve(hydrology.cells.size());
    for (const auto& cell : hydrology.cells)
    {
        result.bedElevationsMeters.push_back(cell.elevationMeters);
    }
    result.depthsMeters.resize(hydrology.cells.size(), 0.0F);
    result.bankInfluence.resize(hydrology.cells.size(), 0U);

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
                seed.depressionFillMeters) <= 0.0)
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
                            depressionFillMeters) <= 0.0)
                {
                    continue;
                }

                visited[
                    neighborIndex] = 1U;

                frontier.push(
                    neighborIndex);
            }
        }

        const auto deepCells = std::count_if(component.begin(), component.end(),
            [&](const u32 index)
            {
                return basinSurface - hydrology.cells[index].elevationMeters >=
                    config.minimumWaterDepthMeters;
            });
        if (deepCells < static_cast<std::ptrdiff_t>(config.minimumCellsPerBasin))
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

            const auto& cell =
                hydrology.cells[
                    index];

            const f64 terrainElevation =
                static_cast<f64>(
                    cell.elevationMeters);

            const f64 depth =
                basinSurface -
                terrainElevation;

            if (depth <= 0.0)
            {
                continue;
            }

            result.depthsMeters[index] = static_cast<f32>(depth);
            // Include the dry neighbours: sub-grid detail must not resurrect
            // mountains through a basin computed from a lower-bandwidth bed.
            for (i32 oy = -1; oy <= 1; ++oy)
            {
                for (i32 ox = -1; ox <= 1; ++ox)
                {
                    const i32 nx = static_cast<i32>(x) + ox;
                    const i32 ny = static_cast<i32>(y) + oy;
                    if (IsInside(nx, resolution) && IsInside(ny, resolution))
                    {
                        result.bankInfluence[CellIndex(resolution,
                            static_cast<u32>(nx), static_cast<u32>(ny))] = 1U;
                    }
                }
            }
            if (!IsCoreOwned(offset, coreHalfExtentMeters))
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

LakeWaterField BuildLakeWaterField(
    const terrain_hydrology::DrainagePage& drainage,
    const terrain_material_column::MaterialColumnPage& material,
    const LakeWaterConfig config)
{
    const u32 resolution = drainage.Resolution();
    const std::size_t cellCount = static_cast<std::size_t>(resolution) * resolution;
    if (resolution < 3U || material.Resolution() != resolution ||
        std::abs(material.SpacingMeters() - drainage.SpacingMeters()) > 1.0e-9 ||
        !std::isfinite(config.minimumWaterDepthMeters) ||
        config.minimumWaterDepthMeters <= 0.0 || config.minimumCellsPerBasin == 0U)
        throw std::invalid_argument("M09 lake extraction requires matching valid physical pages.");

    LakeWaterField result{};
    result.cellSpacingMeters = drainage.SpacingMeters();
    result.coreHalfExtentMeters =
        static_cast<f64>(resolution - 1U) * 0.5 * drainage.SpacingMeters();
    result.resolution = resolution;
    result.bedElevationsMeters.resize(cellCount, 0.0F);
    result.depthsMeters.resize(cellCount, 0.0F);
    result.bankInfluence.resize(cellCount, 0U);
    for (u32 y = 0U; y < resolution; ++y)
        for (u32 x = 0U; x < resolution; ++x)
            result.bedElevationsMeters[CellIndex(resolution, x, y)] =
                material.At(x, y).SurfaceHeightMeters();

    std::vector<u8> wetCandidates(cellCount, 0U);
    std::vector<u8> visited(cellCount, 0U);
    std::vector<u8> inComponent(cellCount, 0U);
    for (u32 y = 0U; y < resolution; ++y)
        for (u32 x = 0U; x < resolution; ++x)
        {
            const auto& cell = drainage.At(x, y);
            if (!cell.outlet && cell.depressionFillMeters >= config.minimumWaterDepthMeters)
                wetCandidates[CellIndex(resolution, x, y)] = 1U;
        }

    constexpr i32 dx[4]{-1, 1, 0, 0};
    constexpr i32 dy[4]{0, 0, -1, 1};
    std::vector<u32> component;
    std::queue<u32> frontier;
    for (u32 seed = 0U; seed < cellCount; ++seed)
    {
        if (wetCandidates[seed] == 0U || visited[seed] != 0U) continue;
        component.clear();
        frontier.push(seed);
        visited[seed] = 1U;
        f64 lakeSurface = std::numeric_limits<f64>::max();
        u32 minimumCell = seed;
        while (!frontier.empty())
        {
            const u32 index = frontier.front();
            frontier.pop();
            component.push_back(index);
            inComponent[index] = 1U;
            minimumCell = std::min(minimumCell, index);
            const auto& cell = drainage.At(index % resolution, index / resolution);
            lakeSurface = std::min(lakeSurface, static_cast<f64>(cell.drainageElevationMeters));
            const u32 x = index % resolution;
            const u32 y = index / resolution;
            for (u32 side = 0U; side < 4U; ++side)
            {
                const i32 nx = static_cast<i32>(x) + dx[side];
                const i32 ny = static_cast<i32>(y) + dy[side];
                if (!IsInside(nx, resolution) || !IsInside(ny, resolution)) continue;
                const u32 neighbor = static_cast<u32>(CellIndex(
                    resolution, static_cast<u32>(nx), static_cast<u32>(ny)));
                if (wetCandidates[neighbor] == 0U || visited[neighbor] != 0U) continue;
                visited[neighbor] = 1U;
                frontier.push(neighbor);
            }
        }

        u32 spillCell = std::numeric_limits<u32>::max();
        u32 outletCell = std::numeric_limits<u32>::max();
        bool spillExitsPage = false;
        f64 lowestSpillElevation = std::numeric_limits<f64>::max();
        u32 sufficientlyDeepCells = 0U;
        f64 maximumDepth = 0.0;
        for (const u32 index : component)
        {
            const u32 x = index % resolution;
            const u32 y = index / resolution;
            const auto& cell = drainage.At(x, y);
            const f64 bed = result.bedElevationsMeters[index];
            const f64 depth = lakeSurface - bed;
            if (depth >= config.minimumWaterDepthMeters) ++sufficientlyDeepCells;
            if (cell.flow.HasDownstream())
            {
                const i32 nx = static_cast<i32>(x) + cell.flow.dx;
                const i32 ny = static_cast<i32>(y) + cell.flow.dy;
                const bool exits = cell.flow.exitsPage || !IsInside(nx, resolution) || !IsInside(ny, resolution);
                const u32 downstream = exits ? std::numeric_limits<u32>::max() :
                    static_cast<u32>(CellIndex(resolution, static_cast<u32>(nx), static_cast<u32>(ny)));
                if (exits || downstream >= cellCount || inComponent[downstream] == 0U)
                {
                    const f64 candidateElevation = cell.drainageElevationMeters;
                    if (candidateElevation < lowestSpillElevation)
                    {
                        lowestSpillElevation = candidateElevation;
                        spillCell = index;
                        outletCell = exits ? std::numeric_limits<u32>::max() : downstream;
                        spillExitsPage = exits;
                    }
                }
            }
        }
        if (sufficientlyDeepCells < config.minimumCellsPerBasin)
        {
            for (const u32 index : component) inComponent[index] = 0U;
            continue;
        }

        u64 id = terrain::StableCombine64(0x4F52424C414B4531ULL, drainage.SourcePage().address.planet.high);
        id = terrain::StableCombine64(id, drainage.SourcePage().address.planet.low);
        id = terrain::StableCombine64(id, static_cast<u64>(drainage.SourcePage().address.tile.face));
        id = terrain::StableCombine64(id, drainage.SourcePage().address.tile.level);
        id = terrain::StableCombine64(id, drainage.SourcePage().address.tile.x);
        id = terrain::StableCombine64(id, drainage.SourcePage().address.tile.y);
        id = terrain::StableCombine64(id, minimumCell);

        const u32 basinIndex = static_cast<u32>(result.basins.size());
        const u32 firstCell = static_cast<u32>(result.cells.size());
        for (const u32 index : component)
        {
            const f64 depth = lakeSurface - result.bedElevationsMeters[index];
            if (depth <= 0.0) continue;
            result.depthsMeters[index] = static_cast<f32>(depth);
            const u32 x = index % resolution;
            const u32 y = index / resolution;
            for (i32 oy = -1; oy <= 1; ++oy)
                for (i32 ox = -1; ox <= 1; ++ox)
                {
                    const i32 nx = static_cast<i32>(x) + ox;
                    const i32 ny = static_cast<i32>(y) + oy;
                    if (IsInside(nx, resolution) && IsInside(ny, resolution))
                        result.bankInfluence[CellIndex(resolution, static_cast<u32>(nx), static_cast<u32>(ny))] = 1U;
                }
            const math::Double2 offset = CellOffset(resolution, result.cellSpacingMeters, x, y);
            maximumDepth = std::max(maximumDepth, depth);
            result.cells.push_back({
                .sourceCellIndex = index,
                .offsetMeters = offset,
                .terrainElevationMeters = result.bedElevationsMeters[index],
                .surfaceElevationMeters = static_cast<f32>(lakeSurface),
                .depthMeters = static_cast<f32>(depth),
                .basinIndex = basinIndex});
        }
        if (result.cells.size() == firstCell)
        {
            for (const u32 index : component) inComponent[index] = 0U;
            continue;
        }
        const f64 area = static_cast<f64>(result.cells.size() - firstCell) *
            result.cellSpacingMeters * result.cellSpacingMeters;
        result.basins.push_back({
            .surfaceElevationMeters = static_cast<f32>(lakeSurface),
            .maximumDepthMeters = static_cast<f32>(maximumDepth),
            .areaSquareMeters = area,
            .firstCell = firstCell,
            .cellCount = static_cast<u32>(result.cells.size() - firstCell),
            .id = id,
            .spillCellIndex = spillCell,
            .outletCellIndex = outletCell,
            .spillExitsPage = spillExitsPage});
        for (const u32 index : component) inComponent[index] = 0U;
    }
    return result;
}

LakeWaterField BuildLakeWaterField(
    const terrain_hydrology::DrainagePage& drainage,
    const terrain_material_column::MaterialColumnPage& material,
    const terrain_erosion::RiverNetwork& rivers,
    const LakeWaterConfig config)
{
    if (rivers.sourcePage.address != drainage.SourcePage().address ||
        rivers.resolution != drainage.Resolution() ||
        std::abs(rivers.spacingMeters - drainage.SpacingMeters()) > 1.0e-9)
    {
        throw std::invalid_argument(
            "M09 lake basins and M16 river graph must describe the same physical page.");
    }

    LakeWaterField result = BuildLakeWaterField(drainage, material, config);
    const std::size_t cellCount = static_cast<std::size_t>(result.resolution) * result.resolution;
    std::vector<terrain_erosion::RiverNodeId> nodeAtCell(cellCount);
    for (const auto& node : rivers.nodes)
    {
        if (node.sourceX < result.resolution && node.sourceY < result.resolution)
            nodeAtCell[CellIndex(result.resolution, node.sourceX, node.sourceY)] = node.id;
    }

    for (auto& basin : result.basins)
    {
        if (basin.spillExitsPage)
            basin.downstreamRiverExitsPage = true;
        if (basin.outletCellIndex >= cellCount)
            continue;

        u32 cursor = basin.outletCellIndex;
        for (u32 step = 0U; step < cellCount; ++step)
        {
            const auto node = nodeAtCell[cursor];
            if (node.IsValid())
            {
                basin.downstreamRiverNode = node;
                basin.downstreamRiverCellCount = step;
                break;
            }

            const u32 x = cursor % result.resolution;
            const u32 y = cursor / result.resolution;
            const auto& cell = drainage.At(x, y);
            if (!cell.flow.HasDownstream())
                break;
            const i32 nextX = static_cast<i32>(x) + cell.flow.dx;
            const i32 nextY = static_cast<i32>(y) + cell.flow.dy;
            if (cell.flow.exitsPage || !IsInside(nextX, result.resolution) ||
                !IsInside(nextY, result.resolution))
            {
                basin.downstreamRiverExitsPage = true;
                break;
            }
            cursor = static_cast<u32>(CellIndex(
                result.resolution, static_cast<u32>(nextX), static_cast<u32>(nextY)));
        }
    }
    return result;
}

LakeWaterSample SampleLakeWater(
    const LakeWaterField& field, const math::Double2& offsetMeters) noexcept
{
    const auto count = static_cast<std::size_t>(field.resolution) * field.resolution;
    if (field.resolution < 2 || !std::isfinite(field.cellSpacingMeters) ||
        field.cellSpacingMeters <= 0.0 ||
        field.depthsMeters.size() != count || field.bedElevationsMeters.size() != count ||
        field.bankInfluence.size() != count || !std::isfinite(offsetMeters.x) ||
        !std::isfinite(offsetMeters.y))
    {
        return {};
    }
    const f64 half = static_cast<f64>(field.resolution - 1U) * 0.5;
    const f64 x = offsetMeters.x / field.cellSpacingMeters + half;
    const f64 y = offsetMeters.y / field.cellSpacingMeters + half;
    if (x < 0.0 || y < 0.0 || x > half * 2.0 || y > half * 2.0)
    {
        return {};
    }
    const u32 ix = std::min(static_cast<u32>(x), field.resolution - 2U);
    const u32 iy = std::min(static_cast<u32>(y), field.resolution - 2U);
    const f64 tx = x - ix;
    const f64 ty = y - iy;
    LakeWaterSample result{};
    f64 wetWeight = 0.0;
    f64 weightedSurface = 0.0;
    for (u32 oy = 0; oy < 2; ++oy)
    {
        for (u32 ox = 0; ox < 2; ++ox)
        {
            const auto index = CellIndex(field.resolution, ix + ox, iy + oy);
            const f64 weight = (ox == 0 ? 1.0 - tx : tx) * (oy == 0 ? 1.0 - ty : ty);
            const f64 influence = weight * field.bankInfluence[index];
            result.influence += influence;
            result.bedElevationMeters += influence * field.bedElevationsMeters[index];
            if (field.depthsMeters[index] > 0.0F)
            {
                wetWeight += weight;
                weightedSurface += weight * (static_cast<f64>(field.bedElevationsMeters[index]) +
                    field.depthsMeters[index]);
            }
        }
    }
    if (result.influence > 0.0)
    {
        result.bedElevationMeters /= result.influence;
        if (wetWeight > 0.0)
        {
            result.depthMeters = std::max(weightedSurface / wetWeight -
                result.bedElevationMeters, 0.0);
        }
    }
    return result;
}
} // namespace orbit::terrain_water
