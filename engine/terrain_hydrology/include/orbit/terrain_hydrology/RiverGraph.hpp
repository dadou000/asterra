#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain_hydrology/HydrologyGrid.hpp>

#include <vector>

namespace orbit::terrain_hydrology
{
struct RiverNode
{
    u32 sourceCellIndex{0};
    math::Double2 offsetMeters{};
    f32 elevationMeters{0.0F};
    f64 drainageAreaSquareMeters{0.0};
    f32 oceanWeight{0.0F};
};

struct RiverSegment
{
    u32 upstreamNode{0};
    u32 downstreamNode{0};
};

struct RiverGraph
{
    std::vector<RiverNode> nodes;
    std::vector<RiverSegment> segments;
};

[[nodiscard]] RiverGraph BuildRiverGraph(
    const HydrologyGrid& grid,
    f64 minimumDrainageAreaSquareMeters);
} // namespace orbit::terrain_hydrology
