#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/jobs/JobSystem.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/paths/PathEvaluation.hpp>
#include <orbit/paths/PathNetwork.hpp>
#include <orbit/paths/PathProfile.hpp>
#include <orbit/time/SimulationTime.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace orbit::paths
{
struct RouteCostSample
{
    bool traversable{true};
    math::Double3 position{};
    f64 additiveCost{0.0};
    f64 cutMeters{0.0};
    f64 fillMeters{0.0};
    f64 waterDepthMeters{0.0};
};

using RouteCostSource =
    std::function<RouteCostSample(
        const math::Double3& nominalPoint)>;

struct RouteDependencyRevisions
{
    u64 terrain{0};
    u64 profile{0};
    u64 costFields{0};
    u64 endpoints{0};

    [[nodiscard]] bool operator==(
        const RouteDependencyRevisions&) const noexcept = default;
};

[[nodiscard]] u64 RouteDependencySignature(
    const RouteDependencyRevisions& revisions) noexcept;

struct RouteSolveRequest
{
    math::Double3 start{};
    math::Double3 end{};
    math::Double3 up{0.0, 1.0, 0.0};
    PathProfile profile{};
    f64 cellSizeMeters{20.0};
    f64 corridorHalfWidthMeters{500.0};
    u64 dependencyRevision{0};
    RouteCostSource costSource;
};

struct RouteProduct
{
    std::vector<math::Double3> points;
    f64 totalCost{0.0};
    f64 lengthMeters{0.0};
    f64 maximumGrade{0.0};
    u64 dependencyRevision{0};
    u64 routeRevision{0};
};

struct RouteFailure
{
    std::string message;
    u64 dependencyRevision{0};
};

// Builds one disposable solve request from an authoritative routed edge.
// Endpoints are evaluated at the supplied simulation time into targetFrame;
// no world-space coordinates are written back into the semantic network.
[[nodiscard]] std::optional<RouteSolveRequest>
PrepareRouteSolveRequest(
    const PathNetworkService& paths,
    scene::ObjectId edge,
    frames::FrameId targetFrame,
    time::SimulationTime atTime,
    const frames::FrameGraph& frames,
    const universe::BodyRegistry& bodies,
    PathProfile profile,
    RouteCostSource costSource,
    RouteDependencyRevisions dependencies,
    f64 cellSizeMeters = 20.0,
    f64 corridorHalfWidthMeters = 500.0,
    const EntitySocketResolver& entityResolver = {},
    std::string* failureReason = nullptr);

[[nodiscard]] std::optional<RouteProduct>
SolveRoute(
    const RouteSolveRequest& request,
    std::string* failureReason = nullptr);

enum class RouteBuildState : u8
{
    Missing,
    Queued,
    Building,
    Ready,
    Failed,
    Dirty
};

struct RouteStatus
{
    RouteBuildState state{RouteBuildState::Missing};
    u64 requestedGeneration{0};
    u64 dependencyRevision{0};
    u64 committedDependencyRevision{0};
    u64 routeRevision{0};
    std::string error;
};

// CPU-resident derived-route cache. The semantic edge remains authoritative;
// route products are disposable and are rebuilt from routing intent plus
// dependency revisions. Completed stale jobs are discarded by generation.
class RoutingService
{
public:
    explicit RoutingService(
        jobs::JobSystem& jobs);
    ~RoutingService();

    RoutingService(
        const RoutingService&) = delete;
    RoutingService& operator=(
        const RoutingService&) = delete;

    void Request(
        scene::ObjectId edge,
        RouteSolveRequest request);

    void Invalidate(
        scene::ObjectId edge,
        u64 dependencyRevision);

    void Remove(
        scene::ObjectId edge);

    void Poll();

    [[nodiscard]] RouteStatus Status(
        scene::ObjectId edge) const;

    [[nodiscard]] const RouteProduct* Product(
        scene::ObjectId edge) const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::paths
