#pragma once
#include <orbit/path_geometry/CorridorSweep.hpp>
#include <functional>
#include <vector>

namespace orbit::path_geometry
{
// Preliminary earthwork quantities for a ground-supported corridor.
// Samples are in one explicitly resolved local frame (no implicit planet
// projection). Returns the terrain's signed elevation ABOVE the road datum
// at the supplied path-relative world position (metres).
using CorridorTerrainOffsetSampler =
    std::function<f64(const math::Double3& position)>;

struct CorridorEarthworkOptions
{
    f64 halfWidthMeters{3.5};
    f64 shoulderMeters{1.0};
    u32 crossSectionSamples{12}; // even; midpoint quadrature across full width
    f64 maximumStationSpacingMeters{20.0};
};

struct CorridorEarthworkStation
{
    f64 stationMeters{0.0};
    f64 cutAreaSquareMeters{0.0};
    f64 fillAreaSquareMeters{0.0};
};

struct CorridorEarthworkEstimate
{
    std::vector<CorridorEarthworkStation> sections;
    f64 cutCubicMeters{0.0};
    f64 fillCubicMeters{0.0};
    f64 evaluatedLengthMeters{0.0};
};

// An order-of-magnitude quantity preview, NOT a construction or soil solver.
// No side effects, no terrain editing. Reuses the path's stable station frame.
[[nodiscard]] CorridorEarthworkEstimate EstimateCorridorEarthworks(
    const std::vector<PathStation>& stations,
    const CorridorTerrainOffsetSampler& terrainAboveDatum,
    CorridorEarthworkOptions options = {});
} // namespace orbit::path_geometry
