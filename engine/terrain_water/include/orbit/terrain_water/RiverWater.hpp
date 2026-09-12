#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain_erosion/RiverCarving.hpp>
#include <orbit/terrain_hydrology/HydrologyGrid.hpp>
#include <orbit/terrain_hydrology/RiverGraph.hpp>
#include <orbit/world/Planet.hpp>

#include <vector>

namespace orbit::terrain_water
{
struct RiverWaterConfig
{
    f64 channelFillFraction{0.82};

    f64 referenceDrainageAreaSquareMeters{
        1'000'000'000.0
    };

    f64 baseDepthMeters{1.4};
    f64 minimumDepthMeters{0.20};
    f64 maximumDepthMeters{12.0};
    f64 depthExponent{0.22};

    f64 bankClearanceMeters{0.15};

    f64 manningRoughness{0.035};
    f64 minimumSlope{0.00002};
    f64 minimumVelocityMetersPerSecond{0.05};
    f64 maximumVelocityMetersPerSecond{8.0};
};

struct RiverWaterNode
{
    u32 sourceRiverNode{0};
    math::Double2 offsetMeters{};

    f32 bedElevationMeters{0.0F};
    f32 surfaceElevationMeters{0.0F};

    f32 halfWidthMeters{0.0F};
    f32 depthMeters{0.0F};

    f64 drainageAreaSquareMeters{0.0};
};

struct RiverWaterSegment
{
    u32 upstreamNode{0};
    u32 downstreamNode{0};

    f32 slope{0.0F};
    f32 velocityMetersPerSecond{0.0F};
};

struct RiverWaterNetwork
{
    world::SurfaceFrame surfaceFrame{};
    f64 coreHalfExtentMeters{0.0};

    std::vector<RiverWaterNode> nodes;
    std::vector<RiverWaterSegment> segments;
};

[[nodiscard]] RiverWaterNetwork BuildRiverWaterNetwork(
    const terrain_hydrology::HydrologyGrid& hydrology,
    const terrain_hydrology::RiverGraph& rivers,
    const terrain_erosion::RiverCarvingField& carving,
    f64 coreHalfExtentMeters,
    RiverWaterConfig config = {});
} // namespace orbit::terrain_water
