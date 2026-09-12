#pragma once

#include <orbit/core/Types.hpp>

#include <vector>

namespace orbit::water_render
{
struct OceanMeshConfig
{
    u32 radialRings{112};
    u32 angularSegments{128};
};

[[nodiscard]] u32 OceanVertexCount(
    const OceanMeshConfig& config);

[[nodiscard]] u32 OceanIndexCount(
    const OceanMeshConfig& config);

[[nodiscard]] std::vector<u32> BuildOceanIndices(
    const OceanMeshConfig& config);
} // namespace orbit::water_render
