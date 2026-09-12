#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain_hydrology/HydrologyGrid.hpp>
#include <orbit/terrain_hydrology/RiverGraph.hpp>
#include <orbit/world/Planet.hpp>

#include <vector>

namespace orbit::terrain_erosion
{
struct RiverCarvingConfig
{
    f64 referenceDrainageAreaSquareMeters{1'000'000.0};

    f64 baseChannelHalfWidthMeters{4.0};
    f64 minimumChannelHalfWidthMeters{1.5};
    f64 maximumChannelHalfWidthMeters{120.0};
    f64 widthExponent{0.32};

    f64 baseDepthMeters{3.0};
    f64 minimumDepthMeters{0.5};
    f64 maximumDepthMeters{40.0};
    f64 depthExponent{0.18};

    f64 valleyWidthMultiplier{4.0};
    f64 minimumBedSlope{0.00015};
    f64 maximumIncisionMeters{180.0};
};

struct RiverCarvingNode
{
    u32 sourceRiverNode{0};
    math::Double2 offsetMeters{};

    f32 sourceElevationMeters{0.0F};
    f32 drainageElevationMeters{0.0F};
    f32 bedElevationMeters{0.0F};

    f32 channelHalfWidthMeters{0.0F};
    f32 valleyHalfWidthMeters{0.0F};

    f64 drainageAreaSquareMeters{0.0};
};

struct RiverCarvingSegment
{
    u32 upstreamNode{0};
    u32 downstreamNode{0};
};

struct RiverCarvingField
{
    world::SurfaceFrame surfaceFrame{};
    f64 halfExtentMeters{0.0};

    std::vector<RiverCarvingNode> nodes;
    std::vector<RiverCarvingSegment> segments;
};

struct RiverCarvingSample
{
    bool active{false};

    f64 targetElevationMeters{0.0};
    f64 distanceToCenterMeters{0.0};
    f64 influence{0.0};
};

[[nodiscard]] RiverCarvingField BuildRiverCarvingField(
    const terrain_hydrology::HydrologyGrid& hydrology,
    const terrain_hydrology::RiverGraph& rivers,
    RiverCarvingConfig config = {});

[[nodiscard]] RiverCarvingSample SampleRiverCarving(
    const RiverCarvingField& field,
    const math::Double2& offsetMeters) noexcept;
} // namespace orbit::terrain_erosion
