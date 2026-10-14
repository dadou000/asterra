#pragma once
#include <orbit/path_geometry/PathDerived.hpp>
#include <orbit/path_geometry/TransportTopology.hpp>
#include <span>
#include <string>
#include <vector>

namespace orbit::path_geometry
{
// Junctions and movement curves are DERIVED, never a competing authored graph.
// The caller resolves semantic node identity and every incident edge's frame.
struct JunctionApproach
{
    scene::ObjectId edge{};
    frames::FrameId frame{};
    TransportMode mode{TransportMode::Road};
    bool atStart{true};
    bool bidirectional{true};
    const PathDerivedProduct* geometry{nullptr};
};
struct JunctionOptions
{
    f64 endpointToleranceMeters{0.15};
    f64 minimumApproachMeters{0.5};
    f64 maximumConnectorMeters{80.0};
    u32 connectorSegments{12};
    u32 maximumApproaches{16};
    u32 maximumMovements{256};
};
enum class JunctionIssue : u8
{
    InvalidInput,
    MixedFrames,
    MismatchedEndpoints,
    MissingGeometry,
    TooManyApproaches,
    TooManyMovements,
    UnsupportedRailTracks,
    GeometryTooShort
};
struct JunctionFinding
{
    JunctionIssue issue{JunctionIssue::InvalidInput};
    scene::ObjectId edge{};
    std::string explanation;
};
struct JunctionMovement
{
    scene::ObjectId incomingEdge{};
    scene::ObjectId outgoingEdge{};
    u32 incomingLane{0};
    u32 outgoingLane{0};
    TransportMode mode{TransportMode::Road};
    // Lane-centre sampled Hermite connection in the authoritative frame.
    // No overlaid collider is implied by this navigation preview.
    std::vector<math::Double3> points;
    f64 lengthMeters{0.0};
};
struct JunctionPlan
{
    scene::ObjectId node{};
    frames::FrameId frame{};
    math::Double3 centre{};
    std::vector<scene::ObjectId> validEdges;
    std::vector<JunctionMovement> movements;
    std::vector<JunctionFinding> findings;
    // Preview-only convex footprint around road approaches. It is not a
    // watertight curb/sidewalk patch and is not published as terrain collision.
    PathMesh roadFootprint;
    [[nodiscard]] bool Valid() const noexcept { return findings.empty(); }
};
// No cross-mode movements. Explicit same-node rail/road level crossings are
// conflicts but never a transfer. All inputs are bounded for UI/RPC use.
[[nodiscard]] JunctionPlan BuildTransportJunction(
    scene::ObjectId node,
    std::span<const JunctionApproach> approaches,
    JunctionOptions options = {});
} // namespace orbit::path_geometry
