#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain_hydrology/HydrologyGrid.hpp>
#include <orbit/world/Planet.hpp>

#include <vector>

namespace orbit::terrain_water
{
struct LakeWaterConfig
{
    f64 minimumWaterDepthMeters{1.0};
    u32 minimumCellsPerBasin{2};
};

struct LakeWaterCell
{
    u32 sourceCellIndex{0};
    math::Double2 offsetMeters{};

    f32 terrainElevationMeters{0.0F};
    f32 surfaceElevationMeters{0.0F};
    f32 depthMeters{0.0F};

    u32 basinIndex{0};
};

struct LakeWaterBasin
{
    f32 surfaceElevationMeters{0.0F};
    f32 maximumDepthMeters{0.0F};

    f64 areaSquareMeters{0.0};

    u32 firstCell{0};
    u32 cellCount{0};
};

struct LakeWaterField
{
    world::SurfaceFrame surfaceFrame{};

    f64 cellSpacingMeters{0.0};
    f64 coreHalfExtentMeters{0.0};

    std::vector<LakeWaterCell> cells;
    std::vector<LakeWaterBasin> basins;
};

[[nodiscard]] LakeWaterField BuildLakeWaterField(
    const terrain_hydrology::HydrologyGrid& hydrology,
    f64 coreHalfExtentMeters,
    LakeWaterConfig config = {});
} // namespace orbit::terrain_water
