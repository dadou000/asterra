#pragma once

#include <orbit/frames/FrameGraph.hpp>
#include <orbit/jobs/JobSystem.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/paths/PathEvaluation.hpp>
#include <orbit/paths/PathProfile.hpp>
#include <orbit/time/SimulationTime.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace orbit::path_routing
{
enum class RouteState : u8
{
    Missing,
    Dirty,
    Building,
    Ready,
    Failed
};

struct RouteProjectedPoint
{
    math::Double3 localMeters{};
    f64 elevationMeters{0.0};
    f64 waterDepthMeters{0.0};
};

using RouteProjector =
    std::function<std::optional<RouteProjectedPoint>(
        const math::Double3& candidate,
        f64 footprintMeters)>;

using RouteCostEvaluator =
    std::function<std::optional<f64>(
        const RouteProjectedPoint& point,
        f64 footprintMeters)>;

using RouteRevisionProvider =
    std::function<u64()>;

struct RouteCostSource
{
    // Stable semantic key. Only routes referencing this key are invalidated
    // when its revision changes.
    std::string key;
    RouteRevisionProvider revision;
    RouteCostEvaluator evaluateCostPerMeter;
};

struct RouteSearchConfig
{
    f64 spacingMeters{25.0};
    f64 corridorHalfWidthMeters{250.0};
    u32 maximumAlongSamples{512};
    u32 maximumLateralSamples{65};
    u32 maximumGridCells{32'768};
};

struct RouteEnvironment
{
    // Invalid means the start anchor's native frame. A terrain domain should
    // normally use the owning body's frame so parent motion is factored out.
    frames::FrameId frame{};

    std::string domainKey{"identity"};
    RouteRevisionProvider domainRevision;

    // Empty means identity projection in the selected route frame.
    RouteProjector projector;
    std::vector<RouteCostSource> costSources;
    RouteSearchConfig search{};
};

struct RoutePlanRequest
{
    paths::PathEdgeRecord edge;
    paths::PathNodeRecord startNode;
    paths::PathNodeRecord endNode;
    paths::PathProfile profile;

    // Content/asset revision for the resolved profile. Profile changes are
    // therefore independent dependencies instead of global route invalidation.
    u64 profileRevision{1};

    time::SimulationTime atTime{};
    RouteEnvironment environment{};
    paths::EntitySocketResolver entityResolver;
};

struct RoutePoint
{
    math::Double3 localMeters{};
    f64 elevationMeters{0.0};
    f64 waterDepthMeters{0.0};

    [[nodiscard]] bool operator==(
        const RoutePoint&) const noexcept = default;
};

struct RouteResult
{
    scene::ObjectId edge{};
    frames::FrameId frame{};
    std::vector<RoutePoint> points;
    f64 totalCost{0.0};
    u64 dependencySignature{0};
    u64 generation{0};
    bool crossFrameEndpoints{false};
};

struct RouteStatus
{
    RouteState state{RouteState::Missing};
    u64 generation{0};
    u64 committedRevision{0};
    u64 dependencySignature{0};
    bool crossFrameEndpoints{false};
    std::string error;
};

// Asynchronous, CPU-authoritative derived-route cache. Requests snapshot
// semantic edge/node/profile intent on the caller thread, then solve immutable
// data on JobSystem workers. Stale completions are discarded by generation
// and dependency signature.
class RoutePlanner
{
public:
    RoutePlanner(
        jobs::JobSystem& jobs,
        const frames::FrameGraph& frames,
        const universe::BodyRegistry& bodies);

    ~RoutePlanner();

    RoutePlanner(const RoutePlanner&) = delete;
    RoutePlanner& operator=(const RoutePlanner&) = delete;

    // Returns true when this call changed the desired route generation.
    [[nodiscard]] bool Request(
        RoutePlanRequest request);

    // Finalizes completed work, detects revision-provider changes and starts
    // newly dirty jobs. Must be called from the owning/main thread.
    void Poll();

    // Explicit semantic/configuration invalidation for one edge only.
    void Invalidate(scene::ObjectId edge);

    // Removes derived state without touching the authoritative PathEdge.
    void Erase(scene::ObjectId edge) noexcept;

    [[nodiscard]] std::optional<RouteStatus>
    Status(scene::ObjectId edge) const;

    [[nodiscard]] const RouteResult*
    Result(scene::ObjectId edge) const noexcept;

    // Deterministic/headless convenience path used by tests/build tooling.
    [[nodiscard]] bool BuildBlocking(
        RoutePlanRequest request);

private:
    struct ResolvedRequest;
    struct PendingBuild;
    struct Entry;

    [[nodiscard]] ResolvedRequest Resolve(
        RoutePlanRequest request) const;

    [[nodiscard]] u64 CurrentSignature(
        const ResolvedRequest& request,
        u64 manualRevision) const noexcept;

    void RefreshDependencies();
    void FinalizeCompleted();
    void Schedule(scene::ObjectId edge);

    jobs::JobSystem& jobs_;
    const frames::FrameGraph& frames_;
    const universe::BodyRegistry& bodies_;

    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::path_routing
