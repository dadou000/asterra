#pragma once

#include <orbit/path_geometry/TransportJunctions.hpp>
#include <span>
#include <string>
#include <vector>

namespace orbit::path_geometry
{
// Disposable construction product: no scene objects are written. All input
// corridor products, junction patch and connector samples use one frame.
struct RoadJunctionBuildOptions
{
    f64 trimDistanceMeters{8.0};
    f64 maximumNormalAngleRadians{0.35};
    f64 maximumJointElevationDifferenceMeters{2.0};
    u32 connectorSegments{12};
    std::size_t maximumInputVertices{65536};
    std::size_t maximumOutputVertices{65536};
    u32 maximumApproaches{8};
};
enum class RoadJunctionBuildIssue : u8
{
    InvalidInput,
    InvalidTopology,
    UnsupportedMode,
    MissingCorridor,
    MismatchedFrame,
    UnsupportedSection,
    TooShort,
    OverlappingApproaches,
    ExcessiveGrade,
    BudgetExceeded
};
struct RoadJunctionBuildFinding
{
    RoadJunctionBuildIssue issue{RoadJunctionBuildIssue::InvalidInput};
    scene::ObjectId edge{};
    std::string message;
};
struct RoadJunctionTrimmedEdge
{
    scene::ObjectId edge{};
    u32 sourceCutStation{0};
    f64 cutStationMeters{0.0};
    bool atStart{true};
    // Compact ALL supported bands (pavement + two shoulders), with no
    // triangles left underneath the generated junction patch.
    PathMesh retainedMesh;
    std::vector<CollisionTriangle> collision;
};
struct RoadJunctionConstruction
{
    scene::ObjectId node{};
    frames::FrameId frame{};
    u64 dependencySignature{0};
    std::vector<RoadJunctionTrimmedEdge> edges;
    PathMesh pavement;
    std::vector<CollisionTriangle> pavementCollision;
    std::vector<JunctionMovement> laneConnectors;
    std::vector<RoadJunctionBuildFinding> findings;
    [[nodiscard]] bool Ready() const noexcept
    {
        return findings.empty() && pavement.indices.size() >= 3 &&
            edges.size() >= 2;
    }
};

// Supported first milestone: <=8 roads meeting at one same-frame node,
// three-column crowned pavement plus two two-column shoulders, with
// consistently oriented local up. Rail, mixed-mode, unsupported cross-section
// topology, acute overlapping cuts and incomplete sources fail explicitly.
// The caller must atomically replace old corridor mesh/collision with
// retainedMesh + pavement: never simply overlay the patch.
[[nodiscard]] RoadJunctionConstruction BuildRoadJunctionConstruction(
    const JunctionPlan& junction,
    std::span<const JunctionApproach> approaches,
    RoadJunctionBuildOptions options = {});

} // namespace orbit::path_geometry
