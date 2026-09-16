#include <orbit/jobs/JobSystem.hpp>
#include <orbit/paths/Routing.hpp>

#include <cmath>
#include <iostream>

#define ORBIT_TEST_CHECK(expression) \
    do \
    { \
        if (!(expression)) \
        { \
            std::cerr << "Routing test failed: " #expression \
                      << " at line " << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

namespace
{
[[nodiscard]] orbit::paths::RouteCostSample
ObstacleField(
    const orbit::math::Double3& nominal)
{
    // The direct corridor is blocked around the middle. A valid route must
    // leave the center line and go around either end of this synthetic wall.
    const bool blocked =
        nominal.x > 80.0 &&
        nominal.x < 120.0 &&
        std::abs(nominal.z) < 35.0;

    return {
        .traversable = !blocked,
        .position = nominal,
        .additiveCost =
            std::abs(nominal.z) > 70.0
                ? 0.5
                : 0.0
    };
}
}

int main()
{
    orbit::paths::PathProfile road{
        .name = "Routing Test Road",
        .kind = orbit::paths::PathProfileKind::Road,
        .widthMeters = 7.0,
        .lanes = 2,
        .minimumRadiusMeters = 0.0,
        .maximumGrade = 0.2,
        .allowBridge = true,
        .allowTunnel = false,
        .terrainCutCost = 2.0,
        .terrainFillCost = 2.0,
        .waterCrossingCost = 10.0
    };

    orbit::paths::RouteSolveRequest request{
        .start = {0.0, 0.0, 0.0},
        .end = {200.0, 0.0, 0.0},
        .up = {0.0, 1.0, 0.0},
        .profile = road,
        .cellSizeMeters = 10.0,
        .corridorHalfWidthMeters = 100.0,
        .dependencyRevision = 7,
        .costSource = ObstacleField
    };

    std::string failure;
    const auto solved =
        orbit::paths::SolveRoute(
            request,
            &failure);

    ORBIT_TEST_CHECK(solved.has_value());
    ORBIT_TEST_CHECK(failure.empty());
    ORBIT_TEST_CHECK(solved->points.size() >= 3);
    ORBIT_TEST_CHECK(solved->dependencyRevision == 7);
    ORBIT_TEST_CHECK(solved->lengthMeters > 200.0);
    ORBIT_TEST_CHECK(solved->maximumGrade <= 0.2);

    bool leftCenterLine = false;

    for (const auto& point : solved->points)
    {
        if (std::abs(point.z) >= 35.0)
        {
            leftCenterLine = true;
        }
    }

    ORBIT_TEST_CHECK(leftCenterLine);

    orbit::paths::RouteSolveRequest impossible = request;
    impossible.costSource =
        [](const orbit::math::Double3& nominal)
        {
            return orbit::paths::RouteCostSample{
                .traversable = nominal.x < 50.0,
                .position = nominal
            };
        };

    failure.clear();
    ORBIT_TEST_CHECK(
        !orbit::paths::SolveRoute(
            impossible,
            &failure).
            has_value());
    ORBIT_TEST_CHECK(!failure.empty());

    orbit::jobs::JobSystem jobs(2);
    orbit::paths::RoutingService routing(jobs);

    const orbit::scene::ObjectId edge{
        .high = 0x1234567812345678ULL,
        .low = 0x8765432187654321ULL
    };

    routing.Request(edge, request);
    ORBIT_TEST_CHECK(
        routing.Status(edge).state ==
        orbit::paths::RouteBuildState::Queued);

    routing.Poll();
    ORBIT_TEST_CHECK(
        routing.Status(edge).state ==
        orbit::paths::RouteBuildState::Building);

    jobs.WaitIdle();
    routing.Poll();

    const auto ready =
        routing.Status(edge);

    ORBIT_TEST_CHECK(
        ready.state ==
        orbit::paths::RouteBuildState::Ready);
    ORBIT_TEST_CHECK(ready.routeRevision > 0);
    ORBIT_TEST_CHECK(
        ready.committedDependencyRevision == 7);
    ORBIT_TEST_CHECK(
        routing.Product(edge) != nullptr);
    ORBIT_TEST_CHECK(
        routing.Product(edge)->routeRevision ==
        ready.routeRevision);

    const orbit::u64 previousRouteRevision =
        ready.routeRevision;

    routing.Invalidate(edge, 8);
    ORBIT_TEST_CHECK(
        routing.Status(edge).state ==
        orbit::paths::RouteBuildState::Dirty);

    routing.Poll();
    jobs.WaitIdle();
    routing.Poll();

    const auto rebuilt =
        routing.Status(edge);

    ORBIT_TEST_CHECK(
        rebuilt.state ==
        orbit::paths::RouteBuildState::Ready);
    ORBIT_TEST_CHECK(
        rebuilt.committedDependencyRevision == 8);
    ORBIT_TEST_CHECK(
        rebuilt.routeRevision >
        previousRouteRevision);

    // Queue one generation and replace it before completion. Only the newest
    // dependency generation may become the committed derived product.
    orbit::paths::RouteSolveRequest stale = request;
    stale.dependencyRevision = 9;
    routing.Request(edge, stale);
    routing.Poll();

    orbit::paths::RouteSolveRequest newest = request;
    newest.dependencyRevision = 10;
    routing.Request(edge, newest);

    jobs.WaitIdle();
    routing.Poll();
    routing.Poll();
    jobs.WaitIdle();
    routing.Poll();

    const auto latest =
        routing.Status(edge);

    ORBIT_TEST_CHECK(
        latest.state ==
        orbit::paths::RouteBuildState::Ready);
    ORBIT_TEST_CHECK(
        latest.committedDependencyRevision == 10);
    ORBIT_TEST_CHECK(
        routing.Product(edge)->dependencyRevision == 10);

    routing.Remove(edge);
    ORBIT_TEST_CHECK(
        routing.Status(edge).state ==
        orbit::paths::RouteBuildState::Missing);

    return 0;
}
