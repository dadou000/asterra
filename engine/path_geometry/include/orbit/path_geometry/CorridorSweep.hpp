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
struct CorridorRefinementOptions
{
    f64 maximumChordErrorMeters{0.025};
    f64 maximumBankStepRadians{0.02};
    f64 maximumSegmentLengthMeters{8.0};
    u32 maximumSubdivisions{10};
};

struct RefinedCorridor
{
    std::vector<PathStation> stations;
    CorridorSweep corridor;
};

// Refine between *authored source stations*. Cubic Hermite positions are
// determined by endpoint position and tangent; this is an approximation of
// the source path, not an exact reconstruction of a lost Bezier/clothoid.
[[nodiscard]] RefinedCorridor RefineCorridor(
    const std::vector<PathStation>& source,
    const CorridorSweep& corridor,
    CorridorRefinementOptions options = {});

[[nodiscard]] PathMesh BuildCorridorMesh(
    const std::vector<PathStation>& stations,
    const CorridorSweep& corridor);
} // namespace orbit::path_geometry
