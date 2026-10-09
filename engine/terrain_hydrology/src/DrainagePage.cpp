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
    // Lowest conditioned elevation this cell may take. Equal to the surface
    // except at shared edge points, where the neighbour's conditioned copy of
    // the same point raises it.
    f64 floorElevationMeters{0.0};
    f64 drainageElevationMeters{0.0};
    f64 authoredDrainage{0.0};

    f64 drainageAreaSquareMeters{0.0};
    f64 dischargeCubicMetersPerSecond{0.0};

    i8 flowDx{0};
    i8 flowDy{0};

    bool available{false};
    bool outlet{false};
    u64 basinTerminalFingerprint{0};

    // Shared-edge halo cell that sits above its twin copy of the shared point
    // in the neighbour's published solution: it drains toward the page, so it
    // cannot be a drain for the page.
    bool upstreamOfSharedEdge{false};
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

[[nodiscard]] u64 BasinTerminalFingerprint(
    const terrain::PhysicalTerrainPageAddress& address,
    const u32 x,
    const u32 y) noexcept
{
    u64 value = terrain::StableCombine64(
        0x4D3039424153494EULL,
        address.planet.high);
    value = terrain::StableCombine64(value, address.planet.low);
    value = terrain::StableCombine64(value, static_cast<u64>(address.tile.face));
    value = terrain::StableCombine64(value, address.tile.level);
    value = terrain::StableCombine64(value, address.tile.x);
    value = terrain::StableCombine64(value, address.tile.y);
    value = terrain::StableCombine64(value, (static_cast<u64>(x) << 32U) | y);
    return value == 0U ? 1U : value;
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

    cell.floorElevationMeters =
        static_cast<f64>(
            boundary.surfaceHeightMeters);

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
    cell.outlet = boundary.outlet;
    cell.basinTerminalFingerprint = boundary.basinTerminalFingerprint;
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
    const f64 minimumDropMeters,
    const bool sharedEdges)
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
    // encodes the neighboring page's solution. With shared edges, an outward
    // cell whose published flow drains into this page is upstream of the
    // shared edge, so seeding it would force the edge above its own tributary
    // and feed the lift back to the neighbour; only the other outward cells
    // are drains.
    const auto seedHalo =
        [&](const bool skipUpstream)
        {
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
                        working[index].available &&
                        !(skipUpstream &&
                          working[index].upstreamOfSharedEdge))
                    {
                        seed(index);
                    }
                }
            }
        };
    seedHalo(sharedEdges);

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

    if (frontier.empty() && sharedEdges)
    {
        // Every outward cell drains into the page: fall back to treating
        // them all as drains rather than leaving the fill without a boundary.
        seedHalo(false);
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
                    target.floorElevationMeters,
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

// Interior padded coordinate of the shared edge point a twin describes.
struct TwinSlot
{
    u32 paddedX{0};
    u32 paddedY{0};
    const DrainageBoundaryCell* cell{nullptr};
    // Direction (in this page's frame) a flow must have a component along to
    // be leaving the twin's page for ours.
    i8 inwardX{0};
    i8 inwardY{0};
};

template <typename Visitor>
void ForEachTwin(
    const DrainagePageHalo& halo,
    const u32 resolution,
    Visitor&& visit)
{
    for (u32 i = 0; i < resolution; ++i)
    {
        if (i < halo.twinNorth.size())
            visit(TwinSlot{i + 1U, 1U, &halo.twinNorth[i], 0, 1});
        if (i < halo.twinEast.size())
            visit(TwinSlot{resolution, i + 1U, &halo.twinEast[i], -1, 0});
        if (i < halo.twinSouth.size())
            visit(TwinSlot{i + 1U, resolution, &halo.twinSouth[i], 0, -1});
        if (i < halo.twinWest.size())
            visit(TwinSlot{1U, i + 1U, &halo.twinWest[i], 1, 0});
    }
    visit(TwinSlot{1U, 1U, &halo.twinCorners[0], 1, 1});
    visit(TwinSlot{resolution, 1U, &halo.twinCorners[1], -1, 1});
    visit(TwinSlot{resolution, resolution, &halo.twinCorners[2], -1, -1});
    visit(TwinSlot{1U, resolution, &halo.twinCorners[3], 1, -1});
}

void ApplySharedEdgeFloors(
    std::vector<WorkingCell>& working,
    const u32 resolution,
    const DrainagePageHalo& halo)
{
    const u32 paddedResolution = resolution + 2U;
    ForEachTwin(halo, resolution, [&](const TwinSlot& slot)
    {
        WorkingCell& cell = working[Index(paddedResolution, slot.paddedX, slot.paddedY)];
        cell.floorElevationMeters = std::max(
            cell.floorElevationMeters,
            static_cast<f64>(slot.cell->conditionedHeightMeters));
    });

    const auto classify =
        [&](const u32 x, const u32 y, const DrainageBoundaryCell& outer)
        {
            // Upstream when the neighbour's published flow for this outward
            // cell drains into this page: it feeds the shared edge rather than
            // draining it.
            const bool upstream =
                (outer.flowDx != 0 || outer.flowDy != 0) &&
                IsInteriorPaddedCoordinate(
                    static_cast<i32>(x) + outer.flowDx,
                    static_cast<i32>(y) + outer.flowDy,
                    resolution);
            working[Index(paddedResolution, x, y)].upstreamOfSharedEdge = upstream;
        };
    for (u32 i = 0; i < resolution; ++i)
    {
        classify(i + 1U, 0U, halo.north[i]);
        classify(paddedResolution - 1U, i + 1U, halo.east[i]);
        classify(i + 1U, paddedResolution - 1U, halo.south[i]);
        classify(0U, i + 1U, halo.west[i]);
    }
    classify(0U, 0U, halo.corners[0]);
    classify(paddedResolution - 1U, 0U, halo.corners[1]);
    classify(paddedResolution - 1U, paddedResolution - 1U, halo.corners[2]);
    classify(0U, paddedResolution - 1U, halo.corners[3]);
}

// A twin that drains across the seam into this page carries water this page's
// own copy of the point cannot see. Flow along the shared edge, or away from
// this page, stays in the twin's own accumulation and is not imported.
void InjectTwinInflows(
    const std::vector<WorkingCell>&,
    DrainagePage& result,
    const DrainagePageHalo& halo)
{
    const u32 resolution = result.Resolution();
    ForEachTwin(halo, resolution, [&](const TwinSlot& slot)
    {
        const DrainageBoundaryCell& twin = *slot.cell;
        if (twin.flowDx == 0 && twin.flowDy == 0)
            return;

        const bool crosses =
            (slot.inwardX != 0 && twin.flowDx == slot.inwardX) ||
            (slot.inwardY != 0 && twin.flowDy == slot.inwardY);
        if (!crosses)
            return;

        const i32 targetX = static_cast<i32>(slot.paddedX) + twin.flowDx;
        const i32 targetY = static_cast<i32>(slot.paddedY) + twin.flowDy;
        if (!IsInteriorPaddedCoordinate(targetX, targetY, resolution))
            return;

        DrainageCell& target = result.At(
            static_cast<u32>(targetX - 1), static_cast<u32>(targetY - 1));
        target.drainageAreaSquareMeters += twin.drainageAreaSquareMeters;
        target.dischargeCubicMetersPerSecond += twin.dischargeCubicMetersPerSecond;
    });
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
            bool bestTargetIsOutlet = false;

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
                    bestTargetIsOutlet = target.outlet;
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
                        resolution),
                .targetIsOutlet = bestTargetIsOutlet
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

void ResolveBasinTerminals(
    DrainagePage& result,
    const std::vector<WorkingCell>& working)
{
    const u32 resolution = result.Resolution();
    const u32 paddedResolution = resolution + 2U;
    std::vector<u32> order(static_cast<std::size_t>(resolution) * resolution);
    std::iota(order.begin(), order.end(), 0U);
    std::stable_sort(order.begin(), order.end(), [&result, resolution](u32 a, u32 b)
    {
        return result.At(a % resolution, a / resolution).drainageElevationMeters <
               result.At(b % resolution, b / resolution).drainageElevationMeters;
    });

    for (const u32 index : order)
    {
        const u32 x = index % resolution;
        const u32 y = index / resolution;
        auto& cell = result.At(x, y);
        if (cell.outlet || !cell.flow.HasDownstream())
        {
            cell.basinTerminalFingerprint =
                BasinTerminalFingerprint(result.SourcePage().address, x, y);
            continue;
        }

        const i32 nx = static_cast<i32>(x) + cell.flow.dx;
        const i32 ny = static_cast<i32>(y) + cell.flow.dy;
        const u32 paddedX = static_cast<u32>(static_cast<i32>(x) + 1 + cell.flow.dx);
        const u32 paddedY = static_cast<u32>(static_cast<i32>(y) + 1 + cell.flow.dy);
        if (cell.flow.exitsPage || nx < 0 || ny < 0 ||
            nx >= static_cast<i32>(resolution) || ny >= static_cast<i32>(resolution))
        {
            cell.basinTerminalFingerprint = working[Index(paddedResolution, paddedX, paddedY)]
                .basinTerminalFingerprint;
        }
        else
        {
            cell.basinTerminalFingerprint = result.At(
                static_cast<u32>(nx), static_cast<u32>(ny)).basinTerminalFingerprint;
        }
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

    if (HasSharedEdgeTwins() &&
        (twinNorth.size() != resolution ||
         twinEast.size() != resolution ||
         twinSouth.size() != resolution ||
         twinWest.size() != resolution ||
         !validRange(twinNorth) ||
         !validRange(twinEast) ||
         !validRange(twinSouth) ||
         !validRange(twinWest) ||
         !validRange(twinCorners)))
    {
        return false;
    }

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

    return CellAsBoundary(x, y);
}

DrainageBoundaryCell DrainagePage::CellAsBoundary(
    const u32 x,
    const u32 y) const
{
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
            cell.flow.dy,
        .outlet = cell.outlet,
        .basinTerminalFingerprint = cell.basinTerminalFingerprint
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

            workingCell.floorElevationMeters =
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

    const bool sharedEdges =
        hasCompleteHalo &&
        halo.HasSharedEdgeTwins();

    if (sharedEdges)
    {
        ApplySharedEdgeFloors(
            working,
            resolution,
            halo);
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
                    minimumDrainageDropMeters),
            sharedEdges);
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

    if (sharedEdges)
    {
        InjectTwinInflows(
            working,
            result,
            halo);
    }
    else
    {
        InjectBoundaryInflows(
            working,
            result);
    }

    AccumulateInternalFlow(
        result);

    ResolveBasinTerminals(
        result,
        working);

    return result;
}
} // namespace orbit::terrain_hydrology
