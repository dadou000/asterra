#pragma once
#include <orbit/path_geometry/PathDerived.hpp>
#include <vector>
#include <optional>

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
// Every key has the same band/vertex topology. Interpolate geometry only;
// topological changes (adding/removing lanes or rails) require junction geometry.
struct CorridorSectionKey
{
    f64 stationMeters{0};
    std::vector<CorridorBand> bands;
};
struct CorridorSweep
{
    std::vector<CorridorBand> bands;
    // Optional per-station banking angle in radians; empty means no bank.
    std::vector<f64> bankRadians;
    std::vector<CorridorSectionKey> sectionKeys;
};
struct CorridorRefinementOptions
{
    f64 maximumChordErrorMeters{0.025};
    f64 maximumBankStepRadians{0.02};
    f64 maximumSegmentLengthMeters{8.0};
    u32 maximumSubdivisions{10};
};

// Non-destructive terrain construction influence, evaluated in the same
// frame as the derived corridor. Terrain samplers may blend this into their
// displacement field without changing the underlying authored height source.
struct CorridorTerrainFootprint
{
    f64 halfWidthMeters{0.0};
    f64 shoulderMeters{0.0};
    f64 blendMeters{0.0};
};
struct CorridorTerrainSample
{
    f64 targetHeightMeters{0.0};
    f64 influence{0.0}; // 0=source terrain, 1=engineered datum
    f64 signedLateralMeters{0.0};
    f64 stationMeters{0.0};
    [[nodiscard]] bool operator==(const CorridorTerrainSample&) const = default;
};
// Works for planar, frame-local height fields. Planetary body-fixed/geodetic
// projection is deliberately not inferred here.
[[nodiscard]] std::optional<CorridorTerrainSample> SampleCorridorTerrain(
    const std::vector<PathStation>& stations,
    const math::Double3& frameLocalPoint,
    const CorridorTerrainFootprint& footprint);

// Apply the nondestructive construction layer to a raw terrain sample.
// The source terrain remains authoritative; the overlay can be recomputed,
// removed, or re-ordered without baking permanent elevation edits.
struct CorridorTerrainBlend
{
    f64 heightMeters{0.0};
    f64 cutMeters{0.0};
    f64 fillMeters{0.0};
    f64 influence{0.0};
};
[[nodiscard]] CorridorTerrainBlend BlendCorridorTerrainHeight(
    f64 sourceHeightMeters,
    const CorridorTerrainSample& sample);

// A bounded frame-local envelope for mapping to physical planetary pages.
// The caller MUST explicitly transform it to the target body's tile space.
struct CorridorTerrainEnvelope
{
    math::Double3 minimum{};
    math::Double3 maximum{};
};
[[nodiscard]] std::optional<CorridorTerrainEnvelope> CorridorFootprintEnvelope(
    const std::vector<PathStation>& stations,
    const CorridorTerrainFootprint& footprint);

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
// Adapter for Orbit's existing path-product cache/renderer. The normal
// BuildPathDerived result remains the source of lanes and navigation;
// the corridor sweep replaces its visual surface and collision triangles.
[[nodiscard]] PathDerivedProduct BuildCorridorDerived(
    const PathCenterline& centerline,
    const paths::PathProfile& profile,
    const CorridorSweep& corridor,
    PathBuildOptions options = {},
    CorridorRefinementOptions refinement = {});
} // namespace orbit::path_geometry
