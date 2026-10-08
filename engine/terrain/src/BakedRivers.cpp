#include <orbit/terrain/BakedRivers.hpp>

#include <orbit/terrain/TerrainContracts.hpp>
#include <orbit/world/Planet.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace orbit::terrain
{
namespace
{
[[nodiscard]] f64 SmoothStep(const f64 x) noexcept
{
    const f64 t = std::clamp(x, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

struct ReachDistance
{
    f64 distanceMeters{0.0};
    f64 parameter{0.0};
};

// Closest point on the reach a-b, taken on the chord and projected back onto
// the sphere: reaches span a hydrology cell (tens to hundreds of kilometres), so
// the raw chord sags hundreds of metres below the surface.
[[nodiscard]] ReachDistance DistanceToReach(
    const math::Double3& p,
    const math::Double3& a,
    const math::Double3& b,
    const f64 radius) noexcept
{
    const math::Double3 ab = b - a;
    const f64 lengthSquared = math::Dot(ab, ab);
    const f64 t = lengthSquared > 0.0
        ? std::clamp(math::Dot(p - a, ab) / lengthSquared, 0.0, 1.0)
        : 0.0;
    const math::Double3 q = math::Normalize(a + ab * t);
    const math::Double3 d = p - q;
    return {std::sqrt(math::Dot(d, d)) * radius, t};
}
} // namespace

u32 BakedRiverNetwork::BucketOf(const math::Double3& direction) noexcept
{
    const world::CubeCoordinate cube = world::UnitDirectionToCube(direction);
    const f64 n = static_cast<f64>(kBucketsPerFaceEdge);
    const u32 x = static_cast<u32>(std::clamp(
        std::floor((cube.uv.x + 1.0) * 0.5 * n), 0.0, n - 1.0));
    const u32 y = static_cast<u32>(std::clamp(
        std::floor((cube.uv.y + 1.0) * 0.5 * n), 0.0, n - 1.0));
    return (static_cast<u32>(cube.face) * kBucketsPerFaceEdge + y) *
               kBucketsPerFaceEdge + x;
}

BakedRiverNetwork BakedRiverNetwork::Build(
    const f64 planetRadiusMeters,
    const u64 recipeHash,
    std::vector<BakedRiverNode> nodes,
    std::vector<BakedRiverSegment> segments)
{
    if (!(planetRadiusMeters > 0.0))
    {
        throw std::invalid_argument("A river network needs a positive planet radius.");
    }
    for (const BakedRiverSegment& segment : segments)
    {
        if (segment.upstream >= nodes.size() || segment.downstream >= nodes.size())
        {
            throw std::invalid_argument("A river reach references a missing node.");
        }
    }

    BakedRiverNetwork result;
    result.radius_ = planetRadiusMeters;
    result.recipeHash_ = recipeHash;
    result.nodes_ = std::move(nodes);
    result.segments_ = std::move(segments);

    f64 maximumHalfWidth = 0.0;
    for (const BakedRiverNode& node : result.nodes_)
    {
        maximumHalfWidth = std::max(maximumHalfWidth, 0.5 * static_cast<f64>(node.widthMeters));
    }
    result.maximumInfluenceMeters_ = maximumHalfWidth * kBakedRiverInfluenceHalfWidths;

    // Bucket grid. Every sample along a reach is registered in the buckets of
    // the points around it out to the influence distance plus one bucket, so a
    // query only ever needs the bucket it falls in.
    const u32 bucketCount = 6U * kBucketsPerFaceEdge * kBucketsPerFaceEdge;
    std::vector<std::vector<u32>> buckets(bucketCount);
    const f64 bucketAngle =
        (std::numbers::pi * 0.5) / static_cast<f64>(kBucketsPerFaceEdge);
    const f64 margin = bucketAngle + result.maximumInfluenceMeters_ / planetRadiusMeters;

    const auto registerPoint = [&](const math::Double3& p, const u32 segmentIndex)
    {
        // Tangent basis at p.
        const math::Double3 helper =
            std::abs(p.y) < 0.9 ? math::Double3{0.0, 1.0, 0.0} : math::Double3{1.0, 0.0, 0.0};
        const math::Double3 t1 = math::Normalize(math::Cross(helper, p));
        const math::Double3 t2 = math::Cross(p, t1);
        const auto add = [&](const math::Double3& q)
        {
            auto& bucket = buckets[BucketOf(math::Normalize(q))];
            if (bucket.empty() || bucket.back() != segmentIndex)
            {
                bucket.push_back(segmentIndex);
            }
        };
        add(p);
        for (int i = 0; i < 8; ++i)
        {
            const f64 angle = static_cast<f64>(i) * std::numbers::pi * 0.25;
            add(p + (t1 * std::cos(angle) + t2 * std::sin(angle)) * margin);
        }
    };

    for (u32 i = 0; i < result.segments_.size(); ++i)
    {
        const math::Double3& a = result.nodes_[result.segments_[i].upstream].direction;
        const math::Double3& b = result.nodes_[result.segments_[i].downstream].direction;
        const f64 angle = std::acos(std::clamp(math::Dot(a, b), -1.0, 1.0));
        const u32 steps = std::max(1U, static_cast<u32>(std::ceil(angle / (bucketAngle * 0.5))));
        for (u32 s = 0; s <= steps; ++s)
        {
            const f64 t = static_cast<f64>(s) / static_cast<f64>(steps);
            registerPoint(math::Normalize(a + (b - a) * t), i);
        }
    }

    result.bucketRanges_.assign(static_cast<std::size_t>(bucketCount) * 2U, 0U);
    for (u32 b = 0; b < bucketCount; ++b)
    {
        auto& bucket = buckets[b];
        std::sort(bucket.begin(), bucket.end());
        bucket.erase(std::unique(bucket.begin(), bucket.end()), bucket.end());
        result.bucketRanges_[static_cast<std::size_t>(b) * 2U] =
            static_cast<u32>(result.bucketSegments_.size());
        result.bucketRanges_[static_cast<std::size_t>(b) * 2U + 1U] =
            static_cast<u32>(bucket.size());
        result.bucketSegments_.insert(
            result.bucketSegments_.end(), bucket.begin(), bucket.end());
    }

    u64 hash = StableCombine64(0x4252495645524E31ULL, recipeHash);
    hash = StableCombine64(hash, std::bit_cast<u64>(planetRadiusMeters));
    hash = StableCombine64(hash, result.nodes_.size());
    hash = StableCombine64(hash, result.segments_.size());
    for (const BakedRiverNode& node : result.nodes_)
    {
        hash = StableCombine64(hash, std::bit_cast<u64>(node.direction.x));
        hash = StableCombine64(hash, std::bit_cast<u64>(node.direction.y));
        hash = StableCombine64(hash, std::bit_cast<u64>(node.direction.z));
        hash = StableCombine64(hash, std::bit_cast<u32>(node.widthMeters));
        hash = StableCombine64(hash, std::bit_cast<u32>(node.depthMeters));
    }
    for (const BakedRiverSegment& segment : result.segments_)
    {
        hash = StableCombine64(hash, (static_cast<u64>(segment.upstream) << 32U) | segment.downstream);
    }
    result.contentHash_ = hash == 0U ? 1U : hash;
    return result;
}

BakedRiverCarve BakedRiverNetwork::Sample(
    const math::Double3& direction,
    const f64 footprintMeters) const noexcept
{
    BakedRiverCarve best{};
    if (segments_.empty())
    {
        return best;
    }

    const u32 bucket = BucketOf(direction);
    const u32 offset = bucketRanges_[static_cast<std::size_t>(bucket) * 2U];
    const u32 count = bucketRanges_[static_cast<std::size_t>(bucket) * 2U + 1U];
    const f64 footprint = std::max(footprintMeters, 1.0);

    for (u32 i = 0; i < count; ++i)
    {
        const BakedRiverSegment& segment = segments_[bucketSegments_[offset + i]];
        const BakedRiverNode& up = nodes_[segment.upstream];
        const BakedRiverNode& down = nodes_[segment.downstream];

        const ReachDistance reach = DistanceToReach(direction, up.direction, down.direction, radius_);
        const f64 t = reach.parameter;
        const f64 width = static_cast<f64>(up.widthMeters) +
            (static_cast<f64>(down.widthMeters) - up.widthMeters) * t;
        const f64 depth = static_cast<f64>(up.depthMeters) +
            (static_cast<f64>(down.depthMeters) - up.depthMeters) * t;

        // A reach far narrower than the footprint averages out to nothing.
        if (width * 20.0 < footprint)
        {
            continue;
        }

        // Footprint filtering: the cut spreads over at least one footprint and
        // loses depth in proportion, conserving the removed volume.
        const f64 halfWidth = std::max(0.5 * width, 0.5 * footprint);
        const f64 amplitude = depth * std::min(1.0, width / std::max(width, footprint));
        const f64 profile = SmoothStep(1.0 - reach.distanceMeters /
            (kBakedRiverInfluenceHalfWidths * halfWidth));
        const f64 carve = amplitude * profile;
        if (carve > static_cast<f64>(best.depthMeters))
        {
            best.depthMeters = static_cast<f32>(carve);
            best.widthMeters = static_cast<f32>(width);
            best.profile = static_cast<f32>(profile);
            best.dischargeCubicMetersPerSecond = static_cast<f32>(
                static_cast<f64>(up.dischargeCubicMetersPerSecond) +
                (static_cast<f64>(down.dischargeCubicMetersPerSecond) -
                 up.dischargeCubicMetersPerSecond) * t);
        }
    }
    return best;
}

std::size_t BakedRiverNetwork::ByteSize() const noexcept
{
    return nodes_.size() * sizeof(BakedRiverNode) +
           segments_.size() * sizeof(BakedRiverSegment) +
           (bucketRanges_.size() + bucketSegments_.size()) * sizeof(u32);
}
} // namespace orbit::terrain
