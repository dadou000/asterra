#include <orbit/terrain_hydrology/DrainagePage.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
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

constexpr std::array<NeighborOffset, 8> kNeighbors{{
    {-1, 0, 1.0},
    {1, 0, 1.0},
    {0, -1, 1.0},
    {0, 1, 1.0},
    {-1, -1, 1.4142135623730951},
    {1, -1, 1.4142135623730951},
    {-1, 1, 1.4142135623730951},
    {1, 1, 1.4142135623730951}
}};

struct WorkingCell
{
    f64 surfaceHeightMeters{0.0};
    f64 drainageElevationMeters{0.0};
    f64 authoredDrainage{0.0};

    f64 drainageAreaSquareMeters{0.0};
    f64 dischargeCubicMetersPerSecond{0.0};

    i8 flowDx{0};
    i8 flowDy{0};

    bool available{false};
    bool outlet{false};
};

struct FloodNode
{
    std::size_t index{0};
    f64 drainageElevationMeters{0.0};
};

struct FloodNodeGreater
{
    [[nodiscard]] bool operator()(
        const FloodNode& a,
        const FloodNode& b) const noexcept
    {
        if (a.drainageElevationMeters != b.drainageElevationMeters)
        {
            return
                a.drainageElevationMeters >
                b.drainageElevationMeters;
        }

        return a.index > b.index;
    }
};

[[nodiscard]] std::size_t Index(
    const u32 width,
    const u32 x,
    const u32 y) noexcept
{
    return
        static_cast<std::size_t>(y) *
            static_cast<std::size_t>(width) +
        static_cast<std::size_t>(x);
}

[[nodiscard]] bool IsInteriorPaddedCoordinate(
    const i32 x,
    const i32 y,
    const u32 resolution) noexcept
{
    return
        x >= 1 &&
        y >= 1 &&
        x <= static_cast<i32>(resolution) &&
        y <= static_cast<i32>(resolution);
}

void AssignBoundaryCell(
    std::vector<WorkingCell>& working,
    const u32 paddedResolution,
    const u32 x,
    const u32 y,
    const DrainageBoundaryCell& boundary)
{
    WorkingCell& cell =
        working[Index(
            paddedResolution,
            x,
            y)];

    cell.surfaceHeightMeters =
        static_cast<f64>(
            boundary.surfaceHeightMeters);

    cell.drainageElevationMeters =
        static_cast<f64>(
            boundary.conditionedHeightMeters);

    cell.authoredDrainage =
        static_cast<f64>(
            boundary.authoredDrainage);

    cell.drainageAreaSquareMeters =
        boundary.drainageAreaSquareMeters;

    cell.dischargeCubicMetersPerSecond =
        boundary.dischargeCubicMetersPerSecond;

    cell.flowDx = boundary.flowDx;
    cell.flowDy = boundary.flowDy;
    cell.available = true;
}

void PopulateHalo(
    std::vector<WorkingCell>& working,
    const u32 resolution,
    const DrainagePageHalo& halo)
{
    const u32 paddedResolution =
        resolution + 2U;

    for (u32 i = 0; i < resolution; ++i)
    {
        AssignBoundaryCell(
            working,
            paddedResolution,
            i + 1U,
            0U,
            halo.north[i]);

        AssignBoundaryCell(
            working,
            paddedResolution,
            paddedResolution - 1U,
            i + 1U,
            halo.east[i]);

        AssignBoundaryCell(
            working,
            paddedResolution,
            i + 1U,
            paddedResolution - 1U,
            halo.south[i]);

        AssignBoundaryCell(
            working,
            paddedResolution,
            0U,
            i + 1U,
            halo.west[i]);
    }

    AssignBoundaryCell(
        working,
        paddedResolution,
        0U,
        0U,
        halo.corners[0]);

    AssignBoundaryCell(
        working,
        paddedResolution,
        paddedResolution - 1U,
        0U,
        halo.corners[1]);

    AssignBoundaryCell(
        working,
        paddedResolution,
        paddedResolution - 1U,
        paddedResolution - 1U,
        halo.corners[2]);

    AssignBoundaryCell(
        working,
        paddedResolution,
        0U,
        paddedResolution - 1U,
        halo.corners[3]);
}

void ConditionDepressions(
    std::vector<WorkingCell>& working,
    const u32 resolution,
    const f64 minimumDropMeters)
{
    const u32 paddedResolution =
        resolution + 2U;

    std::vector<u8> visited(
        working.size(),
        0U);

    std::priority_queue<
        FloodNode,
        std::vector<FloodNode>,
        FloodNodeGreater>
        frontier;

    const auto seed =
        [&working,
         &visited,
         &frontier](
            const std::size_t index)
        {
            if (visited[index] != 0U)
            {
                return;
            }

            visited[index] = 1U;

            frontier.push({
                .index = index,
                .drainageElevationMeters =
                    working[index].
                        drainageElevationMeters
            });
        };

    // Every imported halo cell is a fixed boundary condition, not an
    // automatically-invented outlet. Its conditioned elevation already
    // encodes the neighboring page's solution.
    for (u32 y = 0; y < paddedResolution; ++y)
    {
        for (u32 x = 0; x < paddedResolution; ++x)
        {
            const bool isHalo =
                x == 0U ||
                y == 0U ||
                x + 1U == paddedResolution ||
                y + 1U == paddedResolution;

            const std::size_t index =
                Index(
                    paddedResolution,
                    x,
                    y);

            if (isHalo &&
                working[index].available)
            {
                seed(index);
            }
        }
    }

    // Explicit physical outlets (ocean etc.) are additional fixed seeds.
    for (u32 y = 0; y < resolution; ++y)
    {
        for (u32 x = 0; x < resolution; ++x)
        {
            const std::size_t paddedIndex =
                Index(
                    paddedResolution,
                    x + 1U,
                    y + 1U);

            if (working[paddedIndex].outlet)
            {
                seed(paddedIndex);
            }
        }
    }

    if (frontier.empty())
    {
        throw std::invalid_argument(
            "Orbit M09 depression fill requires fixed halo boundary "
            "conditions or an explicit physical outlet.");
    }

    while (!frontier.empty())
    {
        const FloodNode current =
            frontier.top();

        frontier.pop();

        const u32 currentX =
            static_cast<u32>(
                current.index %
                paddedResolution);

        const u32 currentY =
            static_cast<u32>(
                current.index /
                paddedResolution);

        for (const NeighborOffset& neighbor :
             kNeighbors)
        {
            const i32 nx =
                static_cast<i32>(currentX) +
                neighbor.dx;

            const i32 ny =
                static_cast<i32>(currentY) +
                neighbor.dy;

            if (!IsInteriorPaddedCoordinate(
                    nx,
                    ny,
                    resolution))
            {
                continue;
            }

            const std::size_t neighborIndex =
                Index(
                    paddedResolution,
                    static_cast<u32>(nx),
                    static_cast<u32>(ny));

            if (visited[neighborIndex] != 0U)
            {
                continue;
            }

            WorkingCell& target =
                working[neighborIndex];

            const f64 minimumTargetElevation =
                current.
                    drainageElevationMeters +
                minimumDropMeters *
                    neighbor.distanceScale;

            target.drainageElevationMeters =
                std::max(
                    target.surfaceHeightMeters,
                    minimumTargetElevation);

            visited[neighborIndex] = 1U;

            frontier.push({
                .index = neighborIndex,
                .drainageElevationMeters =
                    target.
                        drainageElevationMeters
            });
        }
    }
}

void RouteFlow(
    std::vector<WorkingCell>& working,
    DrainagePage& result,
    const DrainageRoutingConfig& config)
{
    const u32 resolution =
        result.Resolution();

    const u32 paddedResolution =
        resolution + 2U;

    constexpr f64 tieTolerance = 1.0e-14;

    for (u32 y = 0; y < resolution; ++y)
    {
        for (u32 x = 0; x < resolution; ++x)
        {
            const u32 px = x + 1U;
            const u32 py = y + 1U;

            WorkingCell& source =
                working[Index(
                    paddedResolution,
                    px,
                    py)];

            DrainageCell& output =
                result.At(x, y);

            if (source.outlet)
            {
                continue;
            }

            f64 bestScore = 0.0;
            std::size_t bestIndex =
                std::numeric_limits<
                    std::size_t>::max();

            i8 bestDx = 0;
            i8 bestDy = 0;

            for (const NeighborOffset& neighbor :
                 kNeighbors)
            {
                const i32 nx =
                    static_cast<i32>(px) +
                    neighbor.dx;

                const i32 ny =
                    static_cast<i32>(py) +
                    neighbor.dy;

                if (nx < 0 ||
                    ny < 0 ||
                    nx >=
                        static_cast<i32>(
                            paddedResolution) ||
                    ny >=
                        static_cast<i32>(
                            paddedResolution))
                {
                    continue;
                }

                const std::size_t targetIndex =
                    Index(
                        paddedResolution,
                        static_cast<u32>(nx),
                        static_cast<u32>(ny));

                const WorkingCell& target =
                    working[targetIndex];

                if (!target.available)
                {
                    continue;
                }

                const f64 dropMeters =
                    source.
                        drainageElevationMeters -
                    target.
                        drainageElevationMeters;

                if (dropMeters <= 0.0)
                {
                    continue;
                }

                const f64 slope =
                    dropMeters /
                    (result.SpacingMeters() *
                     neighbor.distanceScale);

                const f64 guidance =
                    std::clamp(
                        target.authoredDrainage,
                        -1.0,
                        1.0);

                const f64 score =
                    slope *
                    (1.0 +
                     static_cast<f64>(
                         config.
                             authoredGuidanceWeight) *
                         guidance);

                const bool better =
                    score >
                        bestScore +
                            tieTolerance ||
                    (std::abs(
                         score -
                         bestScore) <=
                         tieTolerance &&
                     targetIndex < bestIndex);

                if (better)
                {
                    bestScore = score;
                    bestIndex = targetIndex;
                    bestDx = neighbor.dx;
                    bestDy = neighbor.dy;
                }
            }

            if (bestIndex ==
                std::numeric_limits<
                    std::size_t>::max())
            {
                continue;
            }

            source.flowDx = bestDx;
            source.flowDy = bestDy;

            output.flow = {
                .dx = bestDx,
                .dy = bestDy,
                .exitsPage =
                    !IsInteriorPaddedCoordinate(
                        static_cast<i32>(px) +
                            bestDx,
                        static_cast<i32>(py) +
                            bestDy,
                        resolution)
            };
        }
    }
}

void InjectBoundaryInflows(
    const std::vector<WorkingCell>& working,
    DrainagePage& result)
{
    const u32 resolution =
        result.Resolution();

    const u32 paddedResolution =
        resolution + 2U;

    for (u32 y = 0; y < paddedResolution; ++y)
    {
        for (u32 x = 0; x < paddedResolution; ++x)
        {
            const bool isHalo =
                x == 0U ||
                y == 0U ||
                x + 1U == paddedResolution ||
                y + 1U == paddedResolution;

            if (!isHalo)
            {
                continue;
            }

            const WorkingCell& boundary =
                working[Index(
                    paddedResolution,
                    x,
                    y)];

            if (!boundary.available ||
                (boundary.flowDx == 0 &&
                 boundary.flowDy == 0))
            {
                continue;
            }

            const i32 targetX =
                static_cast<i32>(x) +
                boundary.flowDx;

            const i32 targetY =
                static_cast<i32>(y) +
                boundary.flowDy;

            if (!IsInteriorPaddedCoordinate(
                    targetX,
                    targetY,
                    resolution))
            {
                continue;
            }

            DrainageCell& target =
                result.At(
                    static_cast<u32>(
                        targetX - 1),
                    static_cast<u32>(
                        targetY - 1));

            target.drainageAreaSquareMeters +=
                boundary.
                    drainageAreaSquareMeters;

            target.dischargeCubicMetersPerSecond +=
                boundary.
                    dischargeCubicMetersPerSecond;
        }
    }
}

void AccumulateInternalFlow(
    DrainagePage& result)
{
    const u32 resolution =
        result.Resolution();

    std::vector<u32> order(
        static_cast<std::size_t>(
            resolution) *
        resolution);

    std::iota(
        order.begin(),
        order.end(),
        0U);

    std::stable_sort(
        order.begin(),
        order.end(),
        [&result,
         resolution](
            const u32 a,
            const u32 b)
        {
            const u32 ax =
                a % resolution;

            const u32 ay =
                a / resolution;

            const u32 bx =
                b % resolution;

            const u32 by =
                b / resolution;

            const f32 aHeight =
                result.At(
                    ax,
                    ay).
                    drainageElevationMeters;

            const f32 bHeight =
                result.At(
                    bx,
                    by).
                    drainageElevationMeters;

            if (aHeight != bHeight)
            {
                return aHeight > bHeight;
            }

            return a < b;
        });

    for (const u32 index :
         order)
    {
        const u32 x =
            index % resolution;

        const u32 y =
            index / resolution;

        const DrainageCell& source =
            result.At(
                x,
                y);

        if (!source.flow.HasDownstream() ||
            source.flow.exitsPage)
        {
            continue;
        }

        const i32 targetX =
            static_cast<i32>(x) +
            source.flow.dx;

        const i32 targetY =
            static_cast<i32>(y) +
            source.flow.dy;

        if (targetX < 0 ||
            targetY < 0 ||
            targetX >=
                static_cast<i32>(
                    resolution) ||
            targetY >=
                static_cast<i32>(
                    resolution))
        {
            throw std::logic_error(
                "Orbit M09 produced an invalid internal downstream "
                "target.");
        }

        DrainageCell& target =
            result.At(
                static_cast<u32>(
                    targetX),
                static_cast<u32>(
                    targetY));

        target.drainageAreaSquareMeters +=
            source.
                drainageAreaSquareMeters;

        target.dischargeCubicMetersPerSecond +=
            source.
                dischargeCubicMetersPerSecond;
    }
}
} // namespace

bool DrainageRoutingConfig::IsValid() const noexcept
{
    return
        std::isfinite(
            minimumDrainageDropMeters) &&
        minimumDrainageDropMeters >=
            0.0F &&
        std::isfinite(
            authoredGuidanceWeight) &&
        authoredGuidanceWeight >=
            0.0F &&
        authoredGuidanceWeight <
            1.0F;
}

bool DrainageBoundaryCell::IsValid() const noexcept
{
    return
        std::isfinite(
            surfaceHeightMeters) &&
        std::isfinite(
            conditionedHeightMeters) &&
        std::isfinite(
            authoredDrainage) &&
        std::isfinite(
            drainageAreaSquareMeters) &&
        std::isfinite(
            dischargeCubicMetersPerSecond) &&
        conditionedHeightMeters +
                1.0e-4F >=
            surfaceHeightMeters &&
        drainageAreaSquareMeters >=
            0.0 &&
        dischargeCubicMetersPerSecond >=
            0.0;
}

bool DrainagePageHalo::IsComplete(
    const u32 resolution) const noexcept
{
    if (north.size() != resolution ||
        east.size() != resolution ||
        south.size() != resolution ||
        west.size() != resolution)
    {
        return false;
    }

    const auto validRange =
        [](const auto& range)
        {
            return std::all_of(
                range.begin(),
                range.end(),
                [](const DrainageBoundaryCell& cell)
                {
                    return cell.IsValid();
                });
        };

    return
        validRange(north) &&
        validRange(east) &&
        validRange(south) &&
        validRange(west) &&
        validRange(corners);
}

u32 DrainagePage::Resolution() const noexcept
{
    return resolution_;
}

f64 DrainagePage::SpacingMeters() const noexcept
{
    return spacingMeters_;
}

u64 DrainagePage::Revision() const noexcept
{
    return revision_;
}

const terrain::PhysicalTerrainPageKey&
DrainagePage::SourcePage() const noexcept
{
    return sourcePage_;
}

DrainageCell& DrainagePage::At(
    const u32 x,
    const u32 y)
{
    if (x >= resolution_ ||
        y >= resolution_)
    {
        throw std::out_of_range(
            "Orbit M09 drainage coordinate is out of range.");
    }

    return
        cells_[Index(
            resolution_,
            x,
            y)];
}

const DrainageCell& DrainagePage::At(
    const u32 x,
    const u32 y) const
{
    if (x >= resolution_ ||
        y >= resolution_)
    {
        throw std::out_of_range(
            "Orbit M09 drainage coordinate is out of range.");
    }

    return
        cells_[Index(
            resolution_,
            x,
            y)];
}

DrainageBoundaryCell DrainagePage::BoundaryCell(
    const DrainageBoundarySide side,
    const u32 index) const
{
    if (index >= resolution_)
    {
        throw std::out_of_range(
            "Orbit M09 boundary index is out of range.");
    }

    u32 x = 0;
    u32 y = 0;

    switch (side)
    {
    case DrainageBoundarySide::North:
        x = index;
        y = 0;
        break;
    case DrainageBoundarySide::East:
        x = resolution_ - 1U;
        y = index;
        break;
    case DrainageBoundarySide::South:
        x = index;
        y = resolution_ - 1U;
        break;
    case DrainageBoundarySide::West:
        x = 0;
        y = index;
        break;
    }

    const DrainageCell& cell =
        At(
            x,
            y);

    return {
        .surfaceHeightMeters =
            cell.surfaceHeightMeters,
        .conditionedHeightMeters =
            cell.drainageElevationMeters,
        .authoredDrainage =
            cell.authoredDrainage,
        .drainageAreaSquareMeters =
            cell.drainageAreaSquareMeters,
        .dischargeCubicMetersPerSecond =
            cell.dischargeCubicMetersPerSecond,
        .flowDx =
            cell.flow.dx,
        .flowDy =
            cell.flow.dy
    };
}

u64 DrainageRevisionFingerprint(
    const terrain::PhysicalTerrainPageKey& sourcePage,
    const DrainageRoutingConfig& config,
    const u64 haloRevision) noexcept
{
    u64 value =
        terrain::StableCombine64(
            0x4F5242495444524EULL, // "ORBITDRN"
            terrain::PhysicalPageFingerprint(
                sourcePage));

    value = terrain::StableCombine64(
        value,
        static_cast<u64>(
            config.depressionPolicy));

    value = terrain::StableCombine64(
        value,
        std::bit_cast<u32>(
            config.
                minimumDrainageDropMeters));

    value = terrain::StableCombine64(
        value,
        std::bit_cast<u32>(
            config.
                authoredGuidanceWeight));

    value = terrain::StableCombine64(
        value,
        haloRevision);

    return value;
}

DrainagePage BuildDrainagePage(
    const terrain_material_column::MaterialColumnPage& materialColumn,
    const terrain::PhysicalTerrainPageKey& sourcePage,
    const std::span<const DrainageCellInput> inputs,
    const DrainagePageHalo& halo,
    const DrainageRoutingConfig& config)
{
    if (!config.IsValid())
    {
        throw std::invalid_argument(
            "Orbit M09 drainage routing configuration is invalid.");
    }

    const u32 resolution =
        materialColumn.Resolution();

    if (resolution == 0U ||
        sourcePage.resolution !=
            resolution)
    {
        throw std::invalid_argument(
            "Orbit M09 drainage page resolution must match its M08 "
            "physical page identity.");
    }

    const std::size_t cellCount =
        static_cast<std::size_t>(
            resolution) *
        resolution;

    if (inputs.size() != cellCount)
    {
        throw std::invalid_argument(
            "Orbit M09 requires one runoff/guidance input per M08 "
            "material-column cell.");
    }

    const bool hasCompleteHalo =
        halo.IsComplete(
            resolution);

    if (config.depressionPolicy ==
            DepressionRoutingPolicy::
                FillToBoundary &&
        !hasCompleteHalo)
    {
        throw std::invalid_argument(
            "Orbit M09 FillToBoundary requires a complete neighboring "
            "page halo; page edges are not implicit outlets.");
    }

    DrainagePage result;
    result.sourcePage_ = sourcePage;
    result.config_ = config;
    result.revision_ =
        DrainageRevisionFingerprint(
            sourcePage,
            config,
            halo.revision);
    result.resolution_ = resolution;
    result.spacingMeters_ =
        materialColumn.SpacingMeters();

    result.cells_.resize(
        cellCount);

    const u32 paddedResolution =
        resolution + 2U;

    std::vector<WorkingCell> working(
        static_cast<std::size_t>(
            paddedResolution) *
        paddedResolution);

    if (hasCompleteHalo)
    {
        PopulateHalo(
            working,
            resolution,
            halo);
    }

    const f64 cellArea =
        materialColumn.
            CellAreaSquareMeters();

    for (u32 y = 0; y < resolution; ++y)
    {
        for (u32 x = 0; x < resolution; ++x)
        {
            const std::size_t localIndex =
                Index(
                    resolution,
                    x,
                    y);

            const DrainageCellInput& input =
                inputs[localIndex];

            if (!std::isfinite(
                    input.
                        runoffMetersPerSecond) ||
                input.
                    runoffMetersPerSecond <
                    0.0F ||
                !std::isfinite(
                    input.
                        authoredDrainage))
            {
                throw std::invalid_argument(
                    "Orbit M09 runoff and authored drainage inputs must "
                    "be finite; runoff cannot be negative.");
            }

            const f64 surfaceHeight =
                static_cast<f64>(
                    materialColumn.
                        At(x, y).
                        SurfaceHeightMeters());

            WorkingCell& workingCell =
                working[Index(
                    paddedResolution,
                    x + 1U,
                    y + 1U)];

            workingCell.surfaceHeightMeters =
                surfaceHeight;

            workingCell.drainageElevationMeters =
                surfaceHeight;

            workingCell.authoredDrainage =
                static_cast<f64>(
                    input.
                        authoredDrainage);

            workingCell.available = true;
            workingCell.outlet =
                input.outlet;

            DrainageCell& output =
                result.cells_[
                    localIndex];

            output.surfaceHeightMeters =
                static_cast<f32>(
                    surfaceHeight);

            output.drainageElevationMeters =
                static_cast<f32>(
                    surfaceHeight);

            output.authoredDrainage =
                input.authoredDrainage;

            output.drainageAreaSquareMeters =
                cellArea;

            output.dischargeCubicMetersPerSecond =
                static_cast<f64>(
                    input.
                        runoffMetersPerSecond) *
                cellArea;

            output.outlet =
                input.outlet;
        }
    }

    if (config.depressionPolicy ==
        DepressionRoutingPolicy::
            FillToBoundary)
    {
        ConditionDepressions(
            working,
            resolution,
            static_cast<f64>(
                config.
                    minimumDrainageDropMeters));
    }

    for (u32 y = 0; y < resolution; ++y)
    {
        for (u32 x = 0; x < resolution; ++x)
        {
            const WorkingCell& workingCell =
                working[Index(
                    paddedResolution,
                    x + 1U,
                    y + 1U)];

            DrainageCell& output =
                result.At(
                    x,
                    y);

            output.drainageElevationMeters =
                static_cast<f32>(
                    workingCell.
                        drainageElevationMeters);

            output.depressionFillMeters =
                static_cast<f32>(
                    std::max(
                        workingCell.
                            drainageElevationMeters -
                        workingCell.
                            surfaceHeightMeters,
                        0.0));
        }
    }

    RouteFlow(
        working,
        result,
        config);

    InjectBoundaryInflows(
        working,
        result);

    AccumulateInternalFlow(
        result);

    return result;
}
} // namespace orbit::terrain_hydrology
