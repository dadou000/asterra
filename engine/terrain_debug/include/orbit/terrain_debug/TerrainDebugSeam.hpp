#pragma once

#include <orbit/terrain_debug/TerrainDebugField.hpp>
#include <orbit/terrain_debug/TerrainDebugRaster.hpp>

namespace orbit::terrain_debug
{
struct TerrainDebugSeamComparison
{
    TerrainDebugSeamProbe provenance{};

    u32 samplesCompared{0};
    u32 mismatchedSamples{0};

    // Meaningful for scalar/vector fields. Category/boolean/revision/LOD use
    // exact identity and leave this at zero.
    f64 maximumDifference{0.0};

    [[nodiscard]] bool Comparable() const noexcept;
    [[nodiscard]] bool ValuesContinuous() const noexcept;
};

// Compares matching physical edge samples using the cube-neighborhood's
// receiving-edge and reverse-sample mapping. The two views must represent the
// same debug field and their dimensions must agree with their page stamps.
[[nodiscard]] TerrainDebugSeamComparison CompareSeamValues(
    const TerrainDebugPageStamp& page,
    const TerrainDebugRasterView& pageView,
    world::TileEdge edge,
    const TerrainDebugPageStamp* neighbor,
    const TerrainDebugRasterView* neighborView,
    f64 numericTolerance = 1.0e-5);
} // namespace orbit::terrain_debug
