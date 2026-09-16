#include <orbit/fields/FieldRegistry.hpp>
#include <orbit/frames/FrameGraph.hpp>
#include <orbit/jobs/JobSystem.hpp>
#include <orbit/path_routing/RouteDomains.hpp>
#include <orbit/path_routing/RoutePlanner.hpp>
#include <orbit/paths/PathNetwork.hpp>
#include <orbit/surface/SurfaceRegistry.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <atomic>
#include <cmath>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#define ORBIT_TEST_CHECK(expression) \
    do \
    { \
        if (!(expression)) \
        { \
            std::cerr << "PathRouting test failed: " #expression \
                      << " at line " << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

namespace
{
class MutableTerrain final :
    public orbit::terrain::TerrainSource
{
public:
    [[nodiscard]] orbit::terrain::TerrainSample
    Sample(
        const orbit::terrain::TerrainQuery& query)
        const noexcept override
    {
        return {
            .elevationMeters =
                5.0 +
                query.unitDirection.x *
                    2.0,
            .coarseElevationMeters = 5.0,
            .standingWaterDepthMeters =
                query.unitDirection.z >
                        0.03
                    ? 0.5
                    : 0.0
        };
    }

    [[nodiscard]] orbit::u64
    Revision() const noexcept override
    {
        return revision.load();
    }

    std::atomic<orbit::u64> revision{1};
};

[[nodiscard]] orbit::paths::PathProfile
RoadProfile()
{
    return {
        .name = "Routing Test Road",
        .kind =
            orbit::paths::PathProfileKind::Road,
        .widthMeters = 6.0,
        .lanes = 2,
        .minimumRadiusMeters = 0.0,
        .maximumGrade = 1.0,
        .allowBridge = true,
        .allowTunnel = true,
        .terrainCutCost = 1.0,
        .terrainFillCost = 1.0,
        .waterCrossingCost = 2.0
    };
}

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
        .mode =
            orbit::paths::EdgeMode::Routed
    };
}

[[nodiscard]] bool FinishBuild(
    orbit::path_routing::RoutePlanner& planner,
    orbit::jobs::JobSystem& jobs,
    const orbit::scene::ObjectId edge)
{
    for (int iteration = 0;
         iteration < 16;
         ++iteration)
    {
        planner.Poll();

        const auto status =
            planner.Status(edge);

        if (status.has_value() &&
            status->state ==
                orbit::path_routing::
                    RouteState::Ready)
        {
            return true;
        }

        if (status.has_value() &&
            status->state ==
                orbit::path_routing::
                    RouteState::Failed)
        {
            std::cerr
                << "Route failed: "
                << status->error
                << '\n';
            return false;
        }

        jobs.WaitIdle();
    }

    return false;
}
} // namespace

int main()
{
    orbit::jobs::JobSystem jobs(2);
    orbit::frames::FrameGraph frames;

    const auto root =
        frames.CreateRoot();

    const auto moving =
        frames.CreateFrame(
            root,
            [](const orbit::time::SimulationTime time)
            {
                return orbit::math::RigidTransformD{
                    .translation = {
                        static_cast<orbit::f64>(
                            time.microsecondsFromEpoch) /
                            1'000'000.0,
                        0.0,
                        0.0
                    }
                };
            });

    orbit::universe::BodyRegistry bodies(
        frames);

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
            root,
            {0.0, 0.0, 0.0});

    const auto end =
        FrameNode(
            endId,
            network,
            root,
            {100.0, 0.0, 0.0});

    auto revisionA =
        std::make_shared<
            std::atomic<orbit::u64>>(1);

    orbit::path_routing::RouteEnvironment
        obstacleEnvironment{
            .frame = root,
            .domainKey = "identity:test-a",
            .costSources = {
                {
                    .key = "buildability:a",
                    .revision =
                        [revisionA]
                        {
                            return revisionA->
                                load();
                        },
                    .evaluateCostPerMeter =
                        [](
                            const orbit::path_routing::
                                RouteProjectedPoint& point,
                            const orbit::f64)
                            -> std::optional<orbit::f64>
                        {
                            if (point.localMeters.x >
                                    38.0 &&
                                point.localMeters.x <
                                    62.0 &&
                                std::abs(
                                    point.localMeters.z) <
                                    12.0)
                            {
                                return 500.0;
                            }

                            return 0.0;
                        }
                }
            },
            .search = {
                .spacingMeters = 10.0,
                .corridorHalfWidthMeters = 40.0,
                .maximumAlongSamples = 64,
                .maximumLateralSamples = 9,
                .maximumGridCells = 1024
            }
        };

    // Work around aggregate readability without hiding a magic value in
    // the planner implementation itself.
    obstacleEnvironment.search.
        maximumLateralSamples = 9;

    orbit::path_routing::RoutePlanRequest
        requestA{
            .edge =
                RoutedEdge(
                    edgeId,
                    network,
                    startId,
                    endId),
            .startNode = start,
            .endNode = end,
            .profile = RoadProfile(),
            .profileRevision = 1,
            .environment =
                obstacleEnvironment
        };

    ORBIT_TEST_CHECK(
        planner.BuildBlocking(
            requestA));

    const auto* firstRoute =
        planner.Result(edgeId);

    ORBIT_TEST_CHECK(firstRoute != nullptr);
    ORBIT_TEST_CHECK(
        firstRoute->points.size() > 2);

    bool avoidedCenter = false;

    for (const auto& point :
         firstRoute->points)
    {
        if (std::abs(
                point.localMeters.z) >=
            15.0)
        {
            avoidedCenter = true;
            break;
        }
    }

    ORBIT_TEST_CHECK(avoidedCenter);

    const auto firstStatus =
        planner.Status(edgeId);

    ORBIT_TEST_CHECK(
        firstStatus.has_value());

    const orbit::u64
        firstCommittedRevision =
            firstStatus->
                committedRevision;

    // A second route depends on an unrelated source. Updating source A must
    // not invalidate/rebuild B.
    const auto edgeBId =
        orbit::scene::ObjectId::Random();

    auto revisionB =
        std::make_shared<
            std::atomic<orbit::u64>>(1);

    auto requestB =
        requestA;

    requestB.edge =
        RoutedEdge(
            edgeBId,
            network,
            startId,
            endId);

    requestB.environment.domainKey =
        "identity:test-b";
    requestB.environment.costSources = {
        {
            .key = "buildability:b",
            .revision =
                [revisionB]
                {
                    return revisionB->load();
                },
            .evaluateCostPerMeter =
                [](
                    const orbit::path_routing::
                        RouteProjectedPoint&,
                    const orbit::f64)
                    -> std::optional<orbit::f64>
                {
                    return 0.0;
                }
        }
    };

    ORBIT_TEST_CHECK(
        planner.BuildBlocking(
            requestB));

    const orbit::u64
        routeBRevision =
            planner.Status(edgeBId)->
                committedRevision;

    revisionA->store(2);

    planner.Poll();
    jobs.WaitIdle();
    planner.Poll();

    ORBIT_TEST_CHECK(
        planner.Status(edgeId)->
            committedRevision ==
        firstCommittedRevision + 1U);
    ORBIT_TEST_CHECK(
        planner.Status(edgeBId)->
            committedRevision ==
        routeBRevision);

    const orbit::u64
        routeAAfterCostRevision =
            planner.Status(edgeId)->
                committedRevision;

    requestA.profileRevision = 2;

    ORBIT_TEST_CHECK(
        planner.Request(
            requestA));
    ORBIT_TEST_CHECK(
        FinishBuild(
            planner,
            jobs,
            edgeId));

    ORBIT_TEST_CHECK(
        planner.Status(edgeId)->
            committedRevision ==
        routeAAfterCostRevision + 1U);
    ORBIT_TEST_CHECK(
        planner.Status(edgeBId)->
            committedRevision ==
        routeBRevision);

    // Same-frame endpoints stay local even when the parent frame itself is
    // moving. Re-requesting at another simulation time must be a cache hit.
    const auto sameFrameStartId =
        orbit::scene::ObjectId::Random();
    const auto sameFrameEndId =
        orbit::scene::ObjectId::Random();
    const auto sameFrameEdgeId =
        orbit::scene::ObjectId::Random();

    orbit::path_routing::RoutePlanRequest
        sameFrameRequest{
            .edge =
                RoutedEdge(
                    sameFrameEdgeId,
                    network,
                    sameFrameStartId,
                    sameFrameEndId),
            .startNode =
                FrameNode(
                    sameFrameStartId,
                    network,
                    moving,
                    {0.0, 0.0, 0.0}),
            .endNode =
                FrameNode(
                    sameFrameEndId,
                    network,
                    moving,
                    {80.0, 0.0, 0.0}),
            .profile = RoadProfile(),
            .environment = {
                .domainKey =
                    "identity:moving-local",
                .search = {
                    .spacingMeters = 10.0,
                    .corridorHalfWidthMeters = 20.0,
                    .maximumAlongSamples = 32,
                    .maximumLateralSamples = 5,
                    .maximumGridCells = 256
                }
            }
        };

    ORBIT_TEST_CHECK(
        planner.BuildBlocking(
            sameFrameRequest));

    const auto sameFrameBefore =
        planner.Status(
            sameFrameEdgeId);

    ORBIT_TEST_CHECK(
        sameFrameBefore.has_value());

    sameFrameRequest.atTime = {
        .microsecondsFromEpoch =
            10'000'000
    };

    ORBIT_TEST_CHECK(
        !planner.Request(
            sameFrameRequest));

    const auto sameFrameAfter =
        planner.Status(
            sameFrameEdgeId);

    ORBIT_TEST_CHECK(
        sameFrameAfter->
            generation ==
        sameFrameBefore->
            generation);
    ORBIT_TEST_CHECK(
        sameFrameAfter->
            committedRevision ==
        sameFrameBefore->
            committedRevision);

    // Cross-frame endpoints depend on relative frame motion and therefore
    // legitimately request a new generation at another simulation time.
    const auto crossStartId =
        orbit::scene::ObjectId::Random();
    const auto crossEndId =
        orbit::scene::ObjectId::Random();
    const auto crossEdgeId =
        orbit::scene::ObjectId::Random();

    orbit::path_routing::RoutePlanRequest
        crossRequest{
            .edge =
                RoutedEdge(
                    crossEdgeId,
                    network,
                    crossStartId,
                    crossEndId),
            .startNode =
                FrameNode(
                    crossStartId,
                    network,
                    root,
                    {0.0, 0.0, 0.0}),
            .endNode =
                FrameNode(
                    crossEndId,
                    network,
                    moving,
                    {120.0, 0.0, 0.0}),
            .profile = RoadProfile(),
            .environment = {
                .domainKey =
                    "identity:cross-frame",
                .search = {
                    .spacingMeters = 10.0,
                    .corridorHalfWidthMeters = 20.0,
                    .maximumAlongSamples = 64,
                    .maximumLateralSamples = 5,
                    .maximumGridCells = 512
                }
            }
        };

    ORBIT_TEST_CHECK(
        planner.BuildBlocking(
            crossRequest));

    const auto crossBefore =
        planner.Status(crossEdgeId);

    ORBIT_TEST_CHECK(
        crossBefore.has_value());
    ORBIT_TEST_CHECK(
        crossBefore->
            crossFrameEndpoints);

    crossRequest.atTime = {
        .microsecondsFromEpoch =
            5'000'000
    };

    ORBIT_TEST_CHECK(
        planner.Request(
            crossRequest));
    ORBIT_TEST_CHECK(
        FinishBuild(
            planner,
            jobs,
            crossEdgeId));

    ORBIT_TEST_CHECK(
        planner.Status(crossEdgeId)->
            committedRevision ==
        crossBefore->
            committedRevision +
        1U);

    // Terrain domain: route search points are snapped to the actual terrain
    // surface and terrain Revision() selectively invalidates that route.
    const auto system =
        bodies.CreateSystem(
            "Routing System");

    const auto body =
        bodies.CreateBody({
            .system = system,
            .name = "Routing Body",
            .shape =
                orbit::universe::SphereShape{
                    .radiusMeters = 1'000.0
                }
        });

    const auto* bodyRecord =
        bodies.FindBody(body);

    ORBIT_TEST_CHECK(
        bodyRecord != nullptr);

    orbit::surface::SurfaceRegistry
        surfaces(bodies);

    auto terrain =
        std::make_shared<
            MutableTerrain>();

    surfaces.AttachTerrain(
        body,
        terrain);

    orbit::fields::FieldRegistry fields;
    auto fieldRevision =
        std::make_shared<
            std::atomic<orbit::u64>>(1);

    const auto buildabilityField =
        fields.Register({
            .descriptor = {
                .ownerBody = body,
                .name = "buildability",
                .valueKind =
                    orbit::fields::
                        FieldValueKind::Scalar,
                .domain =
                    orbit::fields::
                        FieldDomain::Surface,
                .residency =
                    orbit::fields::
                        FieldResidency::Cpu
            },
            .cpuEvaluator =
                [](
                    const orbit::fields::
                        FieldLocation& location)
                    -> std::optional<
                        orbit::fields::FieldValue>
                {
                    const auto* surface =
                        std::get_if<
                            orbit::fields::
                                SurfaceFieldLocation>(
                                    &location);

                    if (surface == nullptr)
                    {
                        return std::nullopt;
                    }

                    return orbit::fields::
                        FieldValue{
                            surface->
                                    unitDirection.z >
                                    0.05
                                ? 4.0
                                : 0.0
                        };
                },
            .revision =
                [fieldRevision]
                {
                    return fieldRevision->
                        load();
                }
        });

    auto terrainEnvironment =
        orbit::path_routing::
            MakeTerrainSurfaceEnvironment(
                body,
                bodyRecord->frame,
                {},
                frames,
                bodies,
                surfaces,
                {
                    .spacingMeters = 20.0,
                    .corridorHalfWidthMeters = 80.0,
                    .maximumAlongSamples = 64,
                    .maximumLateralSamples = 9,
                    .maximumGridCells = 1024
                });

    terrainEnvironment.costSources.
        push_back(
            orbit::path_routing::
                MakeSurfaceScalarFieldCostSource(
                    "field:buildability",
                    buildabilityField,
                    2.0,
                    body,
                    bodyRecord->frame,
                    {},
                    frames,
                    bodies,
                    fields));

    const auto surfaceStartId =
        orbit::scene::ObjectId::Random();
    const auto surfaceEndId =
        orbit::scene::ObjectId::Random();
    const auto surfaceEdgeId =
        orbit::scene::ObjectId::Random();

    orbit::paths::PathNodeRecord
        surfaceStart{
            .id = surfaceStartId,
            .network = network,
            .name = "Surface Start",
            .anchor =
                orbit::paths::SurfaceAnchor{
                    .body = body,
                    .coordinate = {
                        0.0,
                        -0.08,
                        0.0
                    }
                }
        };

    orbit::paths::PathNodeRecord
        surfaceEnd{
            .id = surfaceEndId,
            .network = network,
            .name = "Surface End",
            .anchor =
                orbit::paths::SurfaceAnchor{
                    .body = body,
                    .coordinate = {
                        0.0,
                        0.08,
                        0.0
                    }
                }
        };

    auto terrainProfile =
        RoadProfile();

    terrainProfile.preferredCostFields = {
        "buildability"
    };

    orbit::path_routing::RoutePlanRequest
        terrainRequest{
            .edge =
                RoutedEdge(
                    surfaceEdgeId,
                    network,
                    surfaceStartId,
                    surfaceEndId),
            .startNode =
                surfaceStart,
            .endNode =
                surfaceEnd,
            .profile =
                terrainProfile,
            .profileRevision = 7,
            .environment =
                terrainEnvironment
        };

    ORBIT_TEST_CHECK(
        planner.BuildBlocking(
            terrainRequest));

    const auto* terrainRoute =
        planner.Result(
            surfaceEdgeId);

    ORBIT_TEST_CHECK(
        terrainRoute != nullptr);
    ORBIT_TEST_CHECK(
        terrainRoute->frame ==
        bodyRecord->frame);
    ORBIT_TEST_CHECK(
        terrainRoute->points.size() >=
        2);

    for (const auto& point :
         terrainRoute->points)
    {
        const orbit::f64 radius =
            orbit::math::Length(
                point.localMeters);

        ORBIT_TEST_CHECK(
            radius > 995.0);
        ORBIT_TEST_CHECK(
            radius < 1'010.0);
    }

    const orbit::u64
        terrainCommitted =
            planner.Status(
                surfaceEdgeId)->
                committedRevision;

    terrain->revision.store(2);

    planner.Poll();
    jobs.WaitIdle();
    planner.Poll();

    ORBIT_TEST_CHECK(
        planner.Status(
            surfaceEdgeId)->
            committedRevision ==
        terrainCommitted + 1U);

    const auto preferred =
        orbit::path_routing::
            MakePreferredSurfaceFieldCosts(
                terrainProfile,
                1.5,
                body,
                bodyRecord->frame,
                {},
                frames,
                bodies,
                fields);

    ORBIT_TEST_CHECK(
        preferred.size() == 1);
    ORBIT_TEST_CHECK(
        preferred[0].revision() == 1);

    fieldRevision->store(2);

    ORBIT_TEST_CHECK(
        preferred[0].revision() == 2);

    return 0;
}
