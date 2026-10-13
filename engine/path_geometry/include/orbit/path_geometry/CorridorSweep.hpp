#pragma once
#include <orbit/path_geometry/PathDerived.hpp>
#include <vector>

namespace orbit::path_geometry
{
// Cross-section coordinates: x=lateral metres; y=height above path datum.
// Multiple non-overlapping bands represent pavement, rails, shoulders, etc.
struct CorridorProfilePoint
{
    f64 lateralMeters{0};
    f64 elevationMeters{0};
};
struct CorridorBand
{
    std::vector<CorridorProfilePoint> profile;
};
struct CorridorSweep
{
    std::vector<CorridorBand> bands;
    // Optional per-station banking angle in radians; empty means no bank.
    std::vector<f64> bankRadians;
};
[[nodiscard]] PathMesh BuildCorridorMesh(
    const std::vector<PathStation>& stations,
    const CorridorSweep& corridor);
} // namespace orbit::path_geometry
