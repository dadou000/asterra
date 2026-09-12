#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/world/Planet.hpp>

#include <vector>

namespace orbit::terrain_hydrology
{
struct HydrologyGridConfig
{
    u32 resolution{65};
    f64 halfExtentMeters{250'000.0};
    f64 footprintMeters{0.0};
    bool useCoarseElevation{true};

    // Removes enclosed sinks from the routing surface while keeping
    // the sampled terrain elevation unchanged for later erosion.
    bool conditionDepressions{true};
    f64 minimumDrainageDropMeters{0.01};
};

struct HydrologyCell
{
    // Authoritative sampled surface used for terrain/erosion.
    f32 elevationMeters{0.0F};

    // Derived monotonically drainable surface used only for routing.
    f32 drainageElevationMeters{0.0F};
    f32 depressionFillMeters{0.0F};

    f32 runoffWeight{1.0F};
    f32 flowAccumulation{0.0F};
    f32 oceanWeight{0.0F};

    i8 flowDx{0};
    i8 flowDy{0};
};

struct HydrologyGrid
{
    HydrologyGridConfig config{};
    world::SurfaceFrame surfaceFrame{};
    f64 spacingMeters{0.0};
    std::vector<HydrologyCell> cells;

    [[nodiscard]] HydrologyCell& At(
        u32 x,
        u32 y);

    [[nodiscard]] const HydrologyCell& At(
        u32 x,
        u32 y) const;

    [[nodiscard]] f64 DrainageAreaSquareMeters(
        u32 x,
        u32 y) const noexcept;
};

[[nodiscard]] HydrologyGrid BuildHydrologyGrid(
    const world::PlanetDefinition& planet,
    const terrain::TerrainSource& source,
    const world::SurfaceFrame& surfaceFrame,
    HydrologyGridConfig config = {});

// Priority-flood conditioning. Boundary and ocean cells are outlets.
// Raw sampled elevation remains untouched.
void ConditionDepressions(
    HydrologyGrid& grid);

void RouteHydrology(
    HydrologyGrid& grid);

[[nodiscard]] std::vector<u32> ExtractRiverCells(
    const HydrologyGrid& grid,
    f64 minimumDrainageAreaSquareMeters);
} // namespace orbit::terrain_hydrology
