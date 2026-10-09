#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

#include <cstddef>
#include <vector>

namespace orbit::terrain
{
// One point of a baked river centerline.
struct BakedRiverNode
{
    // Unit direction on the planet.
    math::Double3 direction{};
    // Terrain surface height at the bake, the bed the channel is cut into.
    f32 elevationMeters{0.0F};
    // Bankfull channel width and depth from the baked discharge.
    f32 widthMeters{0.0F};
    f32 depthMeters{0.0F};
    f32 dischargeCubicMetersPerSecond{0.0F};
    // Index of the mouth node this river drains to (its own index at a mouth).
    u32 basin{0};
};

// Reach between two nodes, flowing from `upstream` to `downstream`.
struct BakedRiverSegment
{
    u32 upstream{0};
    u32 downstream{0};
};

// Channel cut at one point, as a depth to subtract from the terrain.
struct BakedRiverCarve
{
    f32 depthMeters{0.0F};
    f32 widthMeters{0.0F};
    f32 dischargeCubicMetersPerSecond{0.0F};
    // Cross-section weight of the strongest reach at the point: 1 on the
    // centerline, 0.5 at one half-width, 0 outside the influence distance.
    f32 profile{0.0F};
};

// Immutable baked river network: a global graph of centerline nodes and reaches
// with a per-face bucket grid so a terrain sample finds the reaches near it
// without scanning the graph. Terrain evaluates this as a lookup (distance to a
// stored centerline); no drainage or flow is computed while it generates.
class BakedRiverNetwork
{
public:
    static constexpr u32 kFormatVersion = 2;
    static constexpr u32 kBucketsPerFaceEdge = 64;

    BakedRiverNetwork() = default;

    [[nodiscard]] static BakedRiverNetwork Build(
        f64 planetRadiusMeters,
        u64 recipeHash,
        std::vector<BakedRiverNode> nodes,
        std::vector<BakedRiverSegment> segments,
        u32 incisionResolution = 0,
        std::vector<f32> incisionGutter = {});

    [[nodiscard]] bool Empty() const noexcept { return segments_.empty(); }
    [[nodiscard]] u64 RecipeHash() const noexcept { return recipeHash_; }
    [[nodiscard]] u64 ContentHash() const noexcept { return contentHash_; }
    [[nodiscard]] f64 PlanetRadiusMeters() const noexcept { return radius_; }
    [[nodiscard]] const std::vector<BakedRiverNode>& Nodes() const noexcept
    {
        return nodes_;
    }
    [[nodiscard]] const std::vector<BakedRiverSegment>& Segments() const noexcept
    {
        return segments_;
    }
    [[nodiscard]] std::size_t ByteSize() const noexcept;

    // Baked stream-power relief change (metres, negative = incision) per cube
    // face texel with a one-texel gutter, (res + 2)^2 per face, bilinear. Zero
    // resolution means no incision field.
    [[nodiscard]] bool HasIncision() const noexcept { return incisionResolution_ != 0U; }
    [[nodiscard]] u32 IncisionResolution() const noexcept { return incisionResolution_; }
    [[nodiscard]] const std::vector<f32>& IncisionGutter() const noexcept
    {
        return incisionGutter_;
    }
    [[nodiscard]] f32 IncisionDeltaMeters(const math::Double3& direction) const noexcept;

    // The channel to cut at `direction`. `footprintMeters` filters the cut
    // like every other terrain feature: a river narrower than the footprint
    // contributes its average depth over it, and far below the footprint it
    // contributes nothing.
    [[nodiscard]] BakedRiverCarve Sample(
        const math::Double3& direction,
        f64 footprintMeters) const noexcept;

    // Flat bucket tables for the GPU generator: per bucket {offset, count}
    // into the segment index list, and the list itself.
    [[nodiscard]] const std::vector<u32>& BucketRanges() const noexcept
    {
        return bucketRanges_;
    }
    [[nodiscard]] const std::vector<u32>& BucketSegments() const noexcept
    {
        return bucketSegments_;
    }
    // Largest distance (metres) at which any reach affects terrain.
    [[nodiscard]] f64 MaximumInfluenceMeters() const noexcept
    {
        return maximumInfluenceMeters_;
    }

private:
    [[nodiscard]] static u32 BucketOf(const math::Double3& direction) noexcept;

    f64 radius_{1.0};
    u64 recipeHash_{0};
    u64 contentHash_{0};
    f64 maximumInfluenceMeters_{0.0};
    std::vector<BakedRiverNode> nodes_;
    std::vector<BakedRiverSegment> segments_;
    // 6 * kBucketsPerFaceEdge^2 buckets, {offset, count} pairs.
    std::vector<u32> bucketRanges_;
    std::vector<u32> bucketSegments_;
    u32 incisionResolution_{0};
    std::vector<f32> incisionGutter_;
};

// Index of texel (x, y) of a face in a gutter raster; x and y may be -1 or
// resolution (the gutter).
[[nodiscard]] constexpr std::size_t BakedGutterIndex(
    const u32 resolution, const u32 face, const i32 x, const i32 y) noexcept
{
    const std::size_t stride = static_cast<std::size_t>(resolution) + 2U;
    return (static_cast<std::size_t>(face) * stride + static_cast<std::size_t>(y + 1)) * stride +
           static_cast<std::size_t>(x + 1);
}

// Channel half-width multiplier within which a reach affects terrain, shared
// by the CPU query and the GPU generator.
inline constexpr f64 kBakedRiverInfluenceHalfWidths = 2.0;
} // namespace orbit::terrain
