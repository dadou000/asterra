#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/frames/FrameGraph.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/paths/PathProfile.hpp>
#include <orbit/scene/ObjectStore.hpp>

#include <vector>

namespace orbit::path_geometry
{
struct PathCenterlineSample
{
    math::Double3 position{};
    math::Double3 up{0.0, 1.0, 0.0};

    [[nodiscard]] bool operator==(
        const PathCenterlineSample&) const noexcept = default;
};

struct PathCenterline
{
    scene::ObjectId edge{};
    frames::FrameId frame{};
    u64 sourceRevision{0};
    std::vector<PathCenterlineSample> samples;

    [[nodiscard]] bool operator==(
        const PathCenterline&) const noexcept = default;
};

struct PathBuildOptions
{
    // All derived products use this common station spacing so visual,
    // collision and AI/reference products cannot drift independently.
    f64 sampleSpacingMeters{4.0};

    [[nodiscard]] bool operator==(
        const PathBuildOptions&) const noexcept = default;
};

struct PathStation
{
    f64 stationMeters{0.0};
    math::Double3 position{};
    math::Double3 tangent{1.0, 0.0, 0.0};
    math::Double3 up{0.0, 1.0, 0.0};
    math::Double3 lateral{0.0, 0.0, 1.0};

    [[nodiscard]] bool operator==(
        const PathStation&) const noexcept = default;
};

struct PathMeshVertex
{
    math::Double3 position{};
    math::Double3 normal{0.0, 1.0, 0.0};
    math::Double2 uv{};

    [[nodiscard]] bool operator==(
        const PathMeshVertex&) const noexcept = default;
};

struct PathMesh
{
    std::vector<PathMeshVertex> vertices;
    std::vector<u32> indices;

    [[nodiscard]] bool operator==(
        const PathMesh&) const noexcept = default;
};

struct LanePoint
{
    f64 stationMeters{0.0};
    math::Double3 position{};
    math::Double3 tangent{1.0, 0.0, 0.0};

    [[nodiscard]] bool operator==(
        const LanePoint&) const noexcept = default;
};

struct LaneReference
{
    u32 laneIndex{0};
    f64 lateralOffsetMeters{0.0};
    std::vector<LanePoint> points;

    [[nodiscard]] bool operator==(
        const LaneReference&) const noexcept = default;
};

struct ReferenceNode
{
    u32 index{0};
    u32 laneIndex{0};
    f64 stationMeters{0.0};
    math::Double3 position{};
    math::Double3 tangent{1.0, 0.0, 0.0};

    [[nodiscard]] bool operator==(
        const ReferenceNode&) const noexcept = default;
};

struct ReferenceEdge
{
    u32 from{0};
    u32 to{0};
    f64 lengthMeters{0.0};
    bool bidirectional{true};

    [[nodiscard]] bool operator==(
        const ReferenceEdge&) const noexcept = default;
};

struct ReferenceGraph
{
    std::vector<ReferenceNode> nodes;
    std::vector<ReferenceEdge> edges;

    [[nodiscard]] bool operator==(
        const ReferenceGraph&) const noexcept = default;
};

struct CollisionTriangle
{
    math::Double3 a{};
    math::Double3 b{};
    math::Double3 c{};

    [[nodiscard]] bool operator==(
        const CollisionTriangle&) const noexcept = default;
};

struct NavSample
{
    f64 stationMeters{0.0};
    math::Double3 position{};
    math::Double3 tangent{1.0, 0.0, 0.0};
    f64 halfWidthMeters{0.0};
    u32 lanes{0};

    [[nodiscard]] bool operator==(
        const NavSample&) const noexcept = default;
};

enum class DebugLineKind : u8
{
    Boundary,
    Centerline,
    Lane
};

struct DebugLine
{
    math::Double3 start{};
    math::Double3 end{};
    DebugLineKind kind{DebugLineKind::Centerline};

    [[nodiscard]] bool operator==(
        const DebugLine&) const noexcept = default;
};

struct PathDerivedProduct
{
    scene::ObjectId edge{};
    frames::FrameId frame{};
    u64 sourceRevision{0};
    u64 buildSignature{0};
    f64 widthMeters{0.0};
    u32 laneCount{0};

    std::vector<PathStation> stations;
    PathMesh visualMesh;
    std::vector<LaneReference> lanes;
    ReferenceGraph referenceGraph;
    std::vector<CollisionTriangle> collision;
    std::vector<NavSample> navigation;
    std::vector<DebugLine> debugLines;

    [[nodiscard]] bool operator==(
        const PathDerivedProduct&) const noexcept = default;
};

// Pure deterministic CPU derivation. No product is project authority and no
// result depends on cache state. Rebuilding from the same centerline/profile/
// options must produce byte-equivalent scalar/vector values.
[[nodiscard]] PathDerivedProduct BuildPathDerived(
    const PathCenterline& centerline,
    const paths::PathProfile& profile,
    PathBuildOptions options = {});
} // namespace orbit::path_geometry
