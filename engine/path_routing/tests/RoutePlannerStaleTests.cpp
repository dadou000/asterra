#include <orbit/frames/FrameGraph.hpp>
#include <orbit/jobs/JobSystem.hpp>
#include <orbit/path_routing/RoutePlanner.hpp>
#include <orbit/paths/PathNetwork.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <atomic>
#include <iostream>
#include <memory>
#include <optional>
#include <thread>

#define ORBIT_TEST_CHECK(expression) \
    do \
    { \
        if (!(expression)) \
        { \
            std::cerr << "PathRouting stale test failed: " #expression \
                      << " at line " << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

namespace
{
[[nodiscard]] orbit::paths::PathNodeRecord
FrameNode(
    const orbit::scene::ObjectId id,
    const orbit::paths::NetworkId network,
    const orbit::frames::FrameId frame,
    const orbit::math::Double3 position)
{
    return {
        .id = id,
        .network = network,
        .name = "Node",
        .anchor =
            orbit::paths::FramePointAnchor{
                .frame = frame,
                .localMeters = position
            }
    };
}

[[nodiscard]] orbit::paths::PathEdgeRecord
RoutedEdge(
    const orbit::scene::ObjectId id,
    const orbit::paths::NetworkId network,
    const orbit::scene::ObjectId start,
    const orbit::scene::ObjectId end)
{
    return {
        .id = id,
        .network = network,
        .startNode = start,
        .endNode = end,
        .mode = orbit::paths::EdgeMode::Routed
    };
}
}

int main()
{
    orbit::jobs::JobSystem jobs(2);
    orbit::frames::FrameGraph frames;
    const auto frame = frames.CreateRoot();
    orbit::universe::BodyRegistry bodies(frames);
    orbit::path_routing::RoutePlanner planner(
        jobs,
        frames,
        bodies);

    const auto network =
        orbit::paths::NetworkId::Random();
    const auto startId =
        orbit::scene::ObjectId::Random();
    const auto endId =
        orbit::scene::ObjectId::Random();
    const auto edgeId =
        orbit::scene::ObjectId::Random();

    const auto start =
        FrameNode(
            startId,
            network,
            frame,
            {0.0, 0.0, 0.0});
    const auto end =
        FrameNode(
            endId,
            network,
            frame,
            {120.0, 0.0, 0.0});

    auto revision =
        std::make_shared<std::atomic<orbit::u64>>(1);
    auto entered =
        std::make_shared<std::atomic<bool>>(false);
    auto release =
        std::make_shared<std::atomic<bool>>(false);
    auto firstGeneration =
        std::make_shared<std::atomic<bool>>(true);

    orbit::path_routing::RouteEnvironment environment{
        .frame = frame,
        .domainKey = "stale-build-test",
        .costSources = {
            {
                .key = "revisioned-cost",
                .revision =
                    [revision]
                    {
                        return revision->load(
                            std::memory_order_acquire);
                    },
                .evaluateCostPerMeter =
                    [entered,
                     release,
                     firstGeneration](
                        const orbit::path_routing::RouteProjectedPoint&,
                        const orbit::f64)
                        -> std::optional<orbit::f64>
                    {
                        if (firstGeneration->exchange(
                                false,
                                std::memory_order_acq_rel))
                        {
                            entered->store(
                                true,
                                std::memory_order_release);

                            while (!release->load(
                                std::memory_order_acquire))
                            {
                                std::this_thread::yield();
                            }
                        }

                        return 0.0;
                    }
            }
        },
        .search = {
            .spacingMeters = 10.0,
            .corridorHalfWidthMeters = 20.0,
            .maximumAlongSamples = 32,
            .maximumLateralSamples = 5,
            .maximumGridCells = 256
        }
    };

    orbit::path_routing::RoutePlanRequest request{
        .edge =
            RoutedEdge(
                edgeId,
                network,
                startId,
                endId),
        .startNode = start,
        .endNode = end,
        .profile = {
            .name = "Stale Test Road",
            .kind = orbit::paths::PathProfileKind::Road,
            .widthMeters = 6.0,
            .lanes = 2,
            .minimumRadiusMeters = 0.0,
            .maximumGrade = 1.0
        },
        .environment = environment
    };

    ORBIT_TEST_CHECK(planner.Request(request));

    // Request() schedules immediately when the edge first becomes dirty.
    for (int spin = 0;
         spin < 100'000 &&
         !entered->load(std::memory_order_acquire);
         ++spin)
    {
        std::this_thread::yield();
    }

    ORBIT_TEST_CHECK(
        entered->load(std::memory_order_acquire));

    const auto before = planner.Status(edgeId);
    ORBIT_TEST_CHECK(before.has_value());
    ORBIT_TEST_CHECK(
        before->state ==
        orbit::path_routing::RouteState::Building);
    const orbit::u64 firstGenerationNumber =
        before->generation;

    // Change a live dependency while generation N is still blocked.
    revision->store(
        2,
        std::memory_order_release);
    planner.Poll();

    const auto invalidated = planner.Status(edgeId);
    ORBIT_TEST_CHECK(invalidated.has_value());
    ORBIT_TEST_CHECK(
        invalidated->generation ==
        firstGenerationNumber + 1U);
    ORBIT_TEST_CHECK(
        invalidated->state ==
        orbit::path_routing::RouteState::Building);

    release->store(
        true,
        std::memory_order_release);
    jobs.WaitIdle();

    // The stale generation must be discarded, then the newer signature must
    // schedule and become the only committed route.
    planner.Poll();

    auto afterStale = planner.Status(edgeId);
    ORBIT_TEST_CHECK(afterStale.has_value());
    ORBIT_TEST_CHECK(
        afterStale->state ==
            orbit::path_routing::RouteState::Building ||
        afterStale->state ==
            orbit::path_routing::RouteState::Dirty);
    ORBIT_TEST_CHECK(
        afterStale->committedRevision == 0);

    for (int iteration = 0; iteration < 8; ++iteration)
    {
        planner.Poll();
        jobs.WaitIdle();

        const auto status = planner.Status(edgeId);
        if (status.has_value() &&
            status->state ==
                orbit::path_routing::RouteState::Ready)
        {
            break;
        }
    }

    const auto ready = planner.Status(edgeId);
    ORBIT_TEST_CHECK(ready.has_value());
    ORBIT_TEST_CHECK(
        ready->state ==
        orbit::path_routing::RouteState::Ready);
    ORBIT_TEST_CHECK(
        ready->generation ==
        firstGenerationNumber + 1U);
    ORBIT_TEST_CHECK(
        ready->committedRevision == 1U);

    const auto* result = planner.Result(edgeId);
    ORBIT_TEST_CHECK(result != nullptr);
    ORBIT_TEST_CHECK(
        result->generation ==
        ready->generation);
    ORBIT_TEST_CHECK(
        result->dependencySignature ==
        ready->dependencySignature);

    return 0;
}
