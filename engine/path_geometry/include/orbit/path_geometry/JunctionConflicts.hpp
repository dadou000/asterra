#pragma once
#include <orbit/path_geometry/TransportJunctions.hpp>
#include <cstddef>
#include <vector>

namespace orbit::path_geometry
{
enum class JunctionConflictKind : u8 { SharedLane, GeometricOverlap };
struct JunctionConflict
{
    u32 firstMovement{0};
    u32 secondMovement{0};
    JunctionConflictKind kind{JunctionConflictKind::GeometricOverlap};
    f64 minimumDistanceMeters{0.0};
};
struct JunctionConflictOptions
{
    // Radii are physical occupancy envelopes of the moving vehicle,
    // not path rendering widths. Pair conflicts when distance < 2*radius.
    f64 roadSweepRadiusMeters{1.1};
    f64 railSweepRadiusMeters{1.3};
    u32 maximumEvaluatedPairs{8192};
    u32 maximumSegmentComparisons{200000};
    u32 maximumConflicts{4096};
};
struct JunctionConflictReport
{
    std::vector<JunctionConflict> conflicts;
    u32 evaluatedPairs{0};
    u32 evaluatedSegmentComparisons{0};
    bool truncated{false};
    bool valid{false};
};
// Uses true 3D segment-to-segment distance. A bridge 5m above an
// intersection does not conflict merely because its XZ footprint crosses.
[[nodiscard]] JunctionConflictReport AnalyzeJunctionConflicts(
    const JunctionPlan& plan,
    JunctionConflictOptions options = {});
} // namespace orbit::path_geometry
