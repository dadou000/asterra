#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain_erosion/RiverNetwork.hpp>
#include <orbit/terrain_hydrology/HydrologyGrid.hpp>
#include <orbit/terrain_hydrology/DrainagePage.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>
#include <orbit/world/Planet.hpp>

#include <limits>
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
    u64 id{0U};
    u32 spillCellIndex{std::numeric_limits<u32>::max()};
    u32 outletCellIndex{std::numeric_limits<u32>::max()};
    bool spillExitsPage{false};
    terrain_erosion::RiverNodeId downstreamRiverNode{};
    u32 downstreamRiverCellCount{0U};
    bool downstreamRiverExitsPage{false};
};

struct LakeWaterField
{
    world::SurfaceFrame surfaceFrame{};

    f64 cellSpacingMeters{0.0};
    f64 coreHalfExtentMeters{0.0};

    std::vector<LakeWaterCell> cells;
    std::vector<LakeWaterBasin> basins;

    // Dense support includes the overlap and a dry bank around each basin.
    // Sample in O(1); no per-frame cell mesh or visibility budget is needed.
    u32 resolution{0};
    std::vector<f32> bedElevationsMeters;
    std::vector<f32> depthsMeters;
    std::vector<u8> bankInfluence;
};

struct LakeWaterSample
{
    f64 bedElevationMeters{0.0};
    f64 depthMeters{0.0};
    f64 influence{0.0};
};

[[nodiscard]] LakeWaterSample SampleLakeWater(
    const LakeWaterField& field, const math::Double2& offsetMeters) noexcept;

[[nodiscard]] LakeWaterField BuildLakeWaterField(
    const terrain_hydrology::HydrologyGrid& hydrology,
    f64 coreHalfExtentMeters,
    LakeWaterConfig config = {});

// Production M09 lake extraction. Depression fill remains derived drainage
// state; the field only carries water depth and basin/spill diagnostics.
[[nodiscard]] LakeWaterField BuildLakeWaterField(
    const terrain_hydrology::DrainagePage& drainage,
    const terrain_material_column::MaterialColumnPage& material,
    LakeWaterConfig config = {});

// Adds a page-local association from each basin's M09 spill outlet to the
// first downstream node in the existing M16 graph. Routing remains M09-owned.
[[nodiscard]] LakeWaterField BuildLakeWaterField(
    const terrain_hydrology::DrainagePage& drainage,
    const terrain_material_column::MaterialColumnPage& material,
    const terrain_erosion::RiverNetwork& rivers,
    LakeWaterConfig config = {});
} // namespace orbit::terrain_water
