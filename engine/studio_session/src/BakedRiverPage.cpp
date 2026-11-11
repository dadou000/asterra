#include "BakedRiverPage.hpp"

#include <orbit/world/Planet.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <optional>
#include <unordered_map>

namespace orbit::studio_session
{
namespace
{
constexpr u64 kBakedNodeDomain = 0x42414B4544524E44ULL;
constexpr u64 kBakedBasinDomain = 0x42414B4544425349ULL;
constexpr u64 kBakedSegmentDomain = 0x42414B4544534547ULL;
constexpr f64 kWaterFillFraction = 0.82;
constexpr f64 kManningRoughness = 0.035;
constexpr u32 kMaximumStepsPerReach = 128U;

// Gnomonic projection of `direction` onto a specific cube face; nullopt when
// the direction is on the far side of that face.
[[nodiscard]] std::optional<math::Double2> ProjectToFace(
    const world::CubeFace face,
    const math::Double3& d) noexcept
{
    f64 major = 0.0;
    math::Double2 uv{};
    switch (face)
    {
    case world::CubeFace::PositiveX: major = d.x; uv = {-d.z, d.y}; break;
    case world::CubeFace::NegativeX: major = -d.x; uv = {d.z, d.y}; break;
    case world::CubeFace::PositiveY: major = d.y; uv = {d.x, -d.z}; break;
    case world::CubeFace::NegativeY: major = -d.y; uv = {d.x, d.z}; break;
    case world::CubeFace::PositiveZ: major = d.z; uv = {d.x, d.y}; break;
    case world::CubeFace::NegativeZ: major = -d.z; uv = {-d.x, d.y}; break;
    }
    if (major <= 1.0e-6)
    {
        return std::nullopt;
    }
    return math::Double2{uv.x / major, uv.y / major};
}

struct PageFrame
{
    world::CubeBounds bounds{};
    u32 resolution{0};
    f64 spacing{0.0};
    f64 extent{0.0};

    // Page-local metres, centred on the page like the drainage-derived network.
    [[nodiscard]] std::optional<math::Double2> ToLocal(
        const math::Double3& direction) const noexcept
    {
        const auto uv = ProjectToFace(bounds.face, direction);
        if (!uv.has_value())
        {
            return std::nullopt;
        }
        const f64 fx = (uv->x - bounds.minimumUv.x) /
            (bounds.maximumUv.x - bounds.minimumUv.x);
        const f64 fy = (uv->y - bounds.minimumUv.y) /
            (bounds.maximumUv.y - bounds.minimumUv.y);
        return math::Double2{(fx - 0.5) * extent, (fy - 0.5) * extent};
    }

    [[nodiscard]] math::Double3 CellDirection(const u32 x, const u32 y) const noexcept
    {
        const f64 denominator = static_cast<f64>(std::max(resolution, 2U) - 1U);
        const f64 u = bounds.minimumUv.x +
            (bounds.maximumUv.x - bounds.minimumUv.x) * static_cast<f64>(x) / denominator;
        const f64 v = bounds.minimumUv.y +
            (bounds.maximumUv.y - bounds.minimumUv.y) * static_cast<f64>(y) / denominator;
        return world::CubeToUnitDirection({.face = bounds.face, .uv = {u, v}});
    }

    [[nodiscard]] std::pair<u32, u32> NearestCell(const math::Double2& local) const noexcept
    {
        const f64 half = static_cast<f64>(resolution - 1U) * 0.5;
        const auto clampCell = [&](const f64 meters)
        {
            return static_cast<u32>(std::clamp(
                std::round(meters / spacing + half), 0.0, static_cast<f64>(resolution - 1U)));
        };
        return {clampCell(local.x), clampCell(local.y)};
    }
};

[[nodiscard]] PageFrame FrameOf(const terrain_hydrology::DrainagePage& drainage)
{
    PageFrame frame;
    frame.bounds = world::TileBounds(drainage.SourcePage().address.tile);
    frame.resolution = drainage.Resolution();
    frame.spacing = drainage.SpacingMeters();
    frame.extent = static_cast<f64>(frame.resolution - 1U) * frame.spacing;
    return frame;
}

// Liang-Barsky against the page square; returns the [t0, t1] interval of a-b
// inside it.
[[nodiscard]] std::optional<std::pair<f64, f64>> ClipToPage(
    const math::Double2& a,
    const math::Double2& b,
    const f64 half) noexcept
{
    f64 t0 = 0.0;
    f64 t1 = 1.0;
    const f64 dx = b.x - a.x;
    const f64 dy = b.y - a.y;
    const auto clip = [&](const f64 p, const f64 q)
    {
        if (std::abs(p) < 1.0e-12)
        {
            return q >= 0.0;
        }
        const f64 r = q / p;
        if (p < 0.0)
        {
            if (r > t1) return false;
            t0 = std::max(t0, r);
        }
        else
        {
            if (r < t0) return false;
            t1 = std::min(t1, r);
        }
        return true;
    };
    if (clip(-dx, a.x + half) && clip(dx, half - a.x) &&
        clip(-dy, a.y + half) && clip(dy, half - a.y) && t1 > t0)
    {
        return std::make_pair(t0, t1);
    }
    return std::nullopt;
}

[[nodiscard]] f64 Lerp(const f64 a, const f64 b, const f64 t) noexcept
{
    return a + (b - a) * t;
}

[[nodiscard]] f64 ManningVelocity(const f64 width, const f64 depth, const f64 slope) noexcept
{
    const f64 area = std::max(width, 0.0) * std::max(depth, 0.0);
    const f64 perimeter = std::max(width, 0.0) + 2.0 * std::max(depth, 0.0);
    const f64 radius = area / std::max(perimeter, 1.0e-6);
    const f64 velocity = radius > 0.0
        ? (1.0 / kManningRoughness) * std::pow(radius, 2.0 / 3.0) *
              std::sqrt(std::max(slope, 0.00002))
        : 0.05;
    return std::clamp(velocity, 0.05, 8.0);
}

template <typename Id>
[[nodiscard]] Id StableId(const u64 domain, const u64 a, const u64 b) noexcept
{
    return {terrain::StableCombine64(domain, a), terrain::StableCombine64(b, domain)};
}

[[nodiscard]] u64 DirectionFingerprint(const math::Double3& d) noexcept
{
    u64 h = kBakedBasinDomain;
    h = terrain::StableCombine64(h, std::bit_cast<u32>(static_cast<f32>(d.x)));
    h = terrain::StableCombine64(h, std::bit_cast<u32>(static_cast<f32>(d.y)));
    h = terrain::StableCombine64(h, std::bit_cast<u32>(static_cast<f32>(d.z)));
    return h;
}
} // namespace

void ApplyBakedRiverDischarge(
    terrain_hydrology::DrainagePage& drainage,
    const terrain::BakedRiverNetwork& baked,
    const std::span<const terrain_hydrology::DrainageCellInput> inputs)
{
    if (baked.Empty())
    {
        return;
    }

    const PageFrame frame = FrameOf(drainage);
    const f64 footprint = std::max(frame.spacing, 1.0);
    for (u32 y = 0U; y < frame.resolution; ++y)
    {
        for (u32 x = 0U; x < frame.resolution; ++x)
        {
            const terrain::BakedRiverCarve carve =
                baked.Sample(frame.CellDirection(x, y), footprint);
            // Inside the bankfull channel only (profile 0.5 is one half-width).
            if (carve.profile < 0.5F || carve.dischargeCubicMetersPerSecond <= 0.0F)
            {
                continue;
            }

            auto& cell = drainage.At(x, y);
            const f64 discharge = carve.dischargeCubicMetersPerSecond;
            if (discharge <= cell.dischargeCubicMetersPerSecond)
            {
                continue;
            }
            cell.dischargeCubicMetersPerSecond = discharge;
            const f64 runoff = static_cast<f64>(
                inputs[static_cast<std::size_t>(y) * frame.resolution + x].runoffMetersPerSecond);
            if (runoff > 0.0)
            {
                cell.drainageAreaSquareMeters =
                    std::max(cell.drainageAreaSquareMeters, discharge / runoff);
            }
        }
    }
}

terrain_erosion::RiverNetwork BuildBakedPageRiverNetwork(
    const terrain::BakedRiverNetwork& baked,
    const terrain_hydrology::DrainagePage& drainage,
    const f64 maximumNodeSpacingMeters)
{
    using terrain_erosion::RiverBasinId;
    using terrain_erosion::RiverNodeId;
    using terrain_erosion::RiverSegmentId;

    terrain_erosion::RiverNetwork result;
    result.sourcePage = drainage.SourcePage();
    result.drainageRevision = drainage.Revision();
    result.resolution = drainage.Resolution();
    result.spacingMeters = drainage.SpacingMeters();
    if (baked.Empty())
    {
        return result;
    }

    const PageFrame frame = FrameOf(drainage);
    const f64 half = 0.5 * frame.extent;
    const f64 maxSpacing = std::max(maximumNodeSpacingMeters, frame.spacing);
    const auto& nodes = baked.Nodes();
    const auto& segments = baked.Segments();

    std::vector<u32> outgoing(nodes.size(), 0U);
    for (const auto& segment : segments)
    {
        ++outgoing[segment.upstream];
    }

    const u64 pageHash = terrain::StableCombine64(
        terrain::StableCombine64(
            static_cast<u64>(frame.bounds.face),
            std::bit_cast<u64>(frame.bounds.minimumUv.x)),
        std::bit_cast<u64>(frame.bounds.minimumUv.y));

    std::unordered_map<u32, u32> sharedNode;

    // Lowest surface in the 3x3 around the point: the channel bed already cut
    // into the terrain by the bake.
    const auto bedHeight = [&](const math::Double2& local)
    {
        const auto [cx, cy] = frame.NearestCell(local);
        f32 lowest = drainage.At(cx, cy).surfaceHeightMeters;
        for (i32 dy = -1; dy <= 1; ++dy)
        {
            for (i32 dx = -1; dx <= 1; ++dx)
            {
                const i32 nx = static_cast<i32>(cx) + dx;
                const i32 ny = static_cast<i32>(cy) + dy;
                if (nx < 0 || ny < 0 || nx >= static_cast<i32>(frame.resolution) ||
                    ny >= static_cast<i32>(frame.resolution))
                {
                    continue;
                }
                lowest = std::min(
                    lowest, drainage.At(static_cast<u32>(nx), static_cast<u32>(ny)).surfaceHeightMeters);
            }
        }
        return lowest;
    };

    for (u32 segmentIndex = 0U; segmentIndex < segments.size(); ++segmentIndex)
    {
        const auto& segment = segments[segmentIndex];
        const auto& up = nodes[segment.upstream];
        const auto& down = nodes[segment.downstream];
        const auto a = frame.ToLocal(up.direction);
        const auto b = frame.ToLocal(down.direction);
        if (!a.has_value() || !b.has_value())
        {
            continue;
        }
        const auto clipped = ClipToPage(*a, *b, half);
        if (!clipped.has_value())
        {
            continue;
        }

        const f64 length = std::hypot(b->x - a->x, b->y - a->y) * (clipped->second - clipped->first);
        const u32 steps = std::clamp(
            static_cast<u32>(std::ceil(length / maxSpacing)), 1U, kMaximumStepsPerReach);
        const bool downstreamIsMouth = outgoing[segment.downstream] == 0U;

        std::vector<u32> chain;
        for (u32 step = 0U; step <= steps; ++step)
        {
            const f64 t = Lerp(clipped->first, clipped->second, static_cast<f64>(step) / steps);
            const math::Double2 local{Lerp(a->x, b->x, t), Lerp(a->y, b->y, t)};

            const bool originalEndpoint =
                (step == 0U && clipped->first <= 0.0) ||
                (step == steps && clipped->second >= 1.0);
            const u32 bakedNode = step == 0U ? segment.upstream : segment.downstream;
            if (originalEndpoint)
            {
                if (const auto found = sharedNode.find(bakedNode); found != sharedNode.end())
                {
                    chain.push_back(found->second);
                    continue;
                }
            }

            const f64 discharge = Lerp(
                up.dischargeCubicMetersPerSecond, down.dischargeCubicMetersPerSecond, t);
            const f64 width = Lerp(up.widthMeters, down.widthMeters, t);
            const f64 depth = Lerp(up.depthMeters, down.depthMeters, t);
            const auto [cx, cy] = frame.NearestCell(local);
            const auto& cell = drainage.At(cx, cy);
            const f64 bed = bedHeight(local);

            const f64 dirX = b->x - a->x;
            const f64 dirY = b->y - a->y;
            const f64 major = std::max(std::abs(dirX), std::abs(dirY));
            const i8 flowDx = major > 0.0 ? static_cast<i8>(std::lround(dirX / major)) : i8{0};
            const i8 flowDy = major > 0.0 ? static_cast<i8>(std::lround(dirY / major)) : i8{0};

            const f64 wetted = depth * kWaterFillFraction;
            const u32 nodeIndex = static_cast<u32>(result.nodes.size());
            const u64 a64 = originalEndpoint
                ? terrain::StableCombine64(kBakedNodeDomain, bakedNode)
                : terrain::StableCombine64(
                      terrain::StableCombine64(pageHash, segmentIndex), step);

            terrain_erosion::RiverNetworkNode node;
            node.id = StableId<RiverNodeId>(kBakedNodeDomain, a64, pageHash);
            node.basin = StableId<RiverBasinId>(
                kBakedBasinDomain, DirectionFingerprint(nodes[up.basin].direction), up.basin);
            node.sourceX = cx;
            node.sourceY = cy;
            node.drainageOffsetMeters = local;
            node.channelOffsetMeters = local;
            node.surfaceHeightMeters = static_cast<f32>(bed);
            node.drainageElevationMeters = cell.drainageElevationMeters;
            node.drainageAreaSquareMeters = cell.drainageAreaSquareMeters;
            node.dischargeCubicMetersPerSecond = discharge;
            node.waterLevelMeters = static_cast<f32>(bed + wetted);
            node.manningRoughness = static_cast<f32>(kManningRoughness);
            node.crossSectionAreaSquareMeters = width * wetted;
            node.channelWidthMeters = static_cast<f32>(width);
            node.channelDepthMeters = static_cast<f32>(depth);
            node.drainageFlowDx = flowDx;
            node.drainageFlowDy = flowDy;
            node.exitsPage = step == steps && clipped->second < 1.0;
            node.outlet = step == steps && clipped->second >= 1.0 && downstreamIsMouth;
            result.nodes.push_back(node);
            if (originalEndpoint)
            {
                sharedNode.emplace(bakedNode, nodeIndex);
            }
            chain.push_back(nodeIndex);
        }

        for (std::size_t i = 0U; i + 1U < chain.size(); ++i)
        {
            auto& from = result.nodes[chain[i]];
            auto& to = result.nodes[chain[i + 1U]];
            const f64 distance = std::max(
                std::hypot(
                    to.channelOffsetMeters.x - from.channelOffsetMeters.x,
                    to.channelOffsetMeters.y - from.channelOffsetMeters.y),
                1.0e-6);
            const f64 slope = std::max(
                (static_cast<f64>(from.waterLevelMeters) - to.waterLevelMeters) / distance, 0.00002);
            from.slope = static_cast<f32>(slope);
            from.velocityMetersPerSecond = static_cast<f32>(ManningVelocity(
                from.channelWidthMeters, from.channelDepthMeters * kWaterFillFraction, slope));
            to.slope = std::max(to.slope, static_cast<f32>(slope));
            to.velocityMetersPerSecond = std::max(to.velocityMetersPerSecond, from.velocityMetersPerSecond);

            terrain_erosion::RiverNetworkSegment segmentOut;
            segmentOut.id = StableId<RiverSegmentId>(
                kBakedSegmentDomain,
                terrain::StableCombine64(from.id.high, to.id.high),
                terrain::StableCombine64(from.id.low, to.id.low));
            segmentOut.basin = from.basin;
            segmentOut.upstreamNode = chain[i];
            segmentOut.downstreamNode = chain[i + 1U];
            segmentOut.slope = static_cast<f32>(slope);
            segmentOut.velocityMetersPerSecond = 0.5F *
                (from.velocityMetersPerSecond + to.velocityMetersPerSecond);
            segmentOut.manningRoughness = from.manningRoughness;
            segmentOut.routingPathMeters = {from.channelOffsetMeters, to.channelOffsetMeters};
            result.segments.push_back(std::move(segmentOut));
        }

        if (!chain.empty() && result.nodes[chain.back()].exitsPage)
        {
            const auto& last = result.nodes[chain.back()];
            result.boundaryLinks.push_back({
                .upstreamNode = last.id,
                .basin = last.basin,
                .flowDx = last.drainageFlowDx,
                .flowDy = last.drainageFlowDy,
                .targetX = static_cast<i32>(last.sourceX) + last.drainageFlowDx,
                .targetY = static_cast<i32>(last.sourceY) + last.drainageFlowDy});
        }
    }

    const u64 revision = terrain::StableCombine64(drainage.Revision(), baked.ContentHash());
    for (const auto& node : result.nodes)
    {
        auto existing = std::find_if(
            result.basins.begin(), result.basins.end(),
            [&node](const terrain_erosion::RiverBasinState& basin) { return basin.id == node.basin; });
        if (existing == result.basins.end())
        {
            result.basins.push_back({
                .id = node.basin,
                .drainageRevision = drainage.Revision(),
                .constraintRevision = 0U,
                .revision = revision});
            existing = result.basins.end() - 1;
        }
        ++existing->nodeCount;
    }
    for (const auto& segment : result.segments)
    {
        for (auto& basin : result.basins)
        {
            if (basin.id == segment.basin)
            {
                ++basin.activeSegmentCount;
                break;
            }
        }
    }
    return result;
}
} // namespace orbit::studio_session
