#include <orbit/frames/FrameGraph.hpp>
#include <orbit/path_geometry/PathSource.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <cmath>
#include <iostream>

#define ORBIT_TEST_CHECK(expression) \
    do \
    { \
        if (!(expression)) \
        { \
            std::cerr \
                << "PathSource test failed: " \
                << #expression \
                << " at line " \
                << __LINE__ \
                << '\n'; \
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
Edge(
    const orbit::scene::ObjectId id,
    const orbit::paths::NetworkId network,
    const orbit::scene::ObjectId start,
    const orbit::scene::ObjectId end,
    const orbit::paths::EdgeMode mode)
{
    return {
        .id = id,
        .network = network,
        .startNode = start,
        .endNode = end,
        .mode = mode
    };
}
}

int main()
{
    orbit::frames::FrameGraph frames;
    const auto root = frames.CreateRoot();

    orbit::universe::BodyRegistry bodies(
        frames);

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
            {20.0, 0.0, 0.0});

    {
        const auto direct =
            orbit::path_geometry::
                BuildPathCenterline({
                    .edge =
                        Edge(
                            edgeId,
                            network,
                            startId,
                            endId,
                            orbit::paths::
                                EdgeMode::Direct),
                    .startNode = start,
                    .endNode = end,
                    .targetFrame = root,
                    .frames = &frames,
                    .bodies = &bodies,
                    .sourceRevision = 9
                });

        ORBIT_TEST_CHECK(
            direct.has_value());
        ORBIT_TEST_CHECK(
            direct->samples.size() == 2);
        ORBIT_TEST_CHECK(
            direct->sourceRevision == 9);
        ORBIT_TEST_CHECK(
            direct->samples.front().
                position.x == 0.0);
        ORBIT_TEST_CHECK(
            direct->samples.back().
                position.x == 20.0);
    }

    {
        auto bezier =
            Edge(
                edgeId,
                network,
                startId,
                endId,
                orbit::paths::
                    EdgeMode::Bezier);

        bezier.startHandleMeters = {
            5.0,
            0.0,
            10.0
        };
        bezier.endHandleMeters = {
            -5.0,
            0.0,
            10.0
        };

        const auto curved =
            orbit::path_geometry::
                BuildPathCenterline({
                    .edge = bezier,
                    .startNode = start,
                    .endNode = end,
                    .targetFrame = root,
                    .frames = &frames,
                    .bodies = &bodies,
                    .curveSampleSpacingMeters =
                        2.0
                });

        ORBIT_TEST_CHECK(
            curved.has_value());
        ORBIT_TEST_CHECK(
            curved->samples.size() > 3);
        ORBIT_TEST_CHECK(
            curved->samples[
                curved->samples.size() /
                2U].
                position.z > 0.0);
    }

    {
        const orbit::path_routing::
            RouteResult route{
                .edge = edgeId,
                .frame = root,
                .points = {
                    {
                        .localMeters = {
                            0.0,
                            0.0,
                            0.0
                        }
                    },
                    {
                        .localMeters = {
                            8.0,
                            0.0,
                            4.0
                        }
                    },
                    {
                        .localMeters = {
                            20.0,
                            0.0,
                            0.0
                        }
                    }
                },
                .dependencySignature =
                    0x1234U,
                .generation = 3
            };

        const auto routed =
            orbit::path_geometry::
                BuildPathCenterline({
                    .edge =
                        Edge(
                            edgeId,
                            network,
                            startId,
                            endId,
                            orbit::paths::
                                EdgeMode::Routed),
                    .startNode = start,
                    .endNode = end,
                    .targetFrame = root,
                    .frames = &frames,
                    .bodies = &bodies,
                    .routed = &route
                });

        ORBIT_TEST_CHECK(
            routed.has_value());
        ORBIT_TEST_CHECK(
            routed->samples.size() == 3);
        ORBIT_TEST_CHECK(
            routed->sourceRevision ==
            0x1234U);
        ORBIT_TEST_CHECK(
            routed->samples[1].
                position.z == 4.0);
    }

    {
        const auto system =
            bodies.CreateSystem(
                "Geometry Test");
        const auto body =
            bodies.CreateBody({
                .system = system,
                .name = "Sphere",
                .shape =
                    orbit::universe::
                        SphereShape{
                            .radiusMeters =
                                1'000.0
                        }
            });

        const auto* record =
            bodies.FindBody(body);

        ORBIT_TEST_CHECK(
            record != nullptr);

        const auto surfaceStartId =
            orbit::scene::ObjectId::Random();
        const auto surfaceEndId =
            orbit::scene::ObjectId::Random();
        const auto surfaceEdgeId =
            orbit::scene::ObjectId::Random();

        const orbit::paths::
            PathNodeRecord surfaceStart{
                .id = surfaceStartId,
                .network = network,
                .name = "Surface A",
                .anchor =
                    orbit::paths::
                        SurfaceAnchor{
                            .body = body,
                            .coordinate = {
                                0.0,
                                0.0,
                                0.0
                            }
                        }
            };

        const orbit::paths::
            PathNodeRecord surfaceEnd{
                .id = surfaceEndId,
                .network = network,
                .name = "Surface B",
                .anchor =
                    orbit::paths::
                        SurfaceAnchor{
                            .body = body,
                            .coordinate = {
                                0.0,
                                0.02,
                                0.0
                            }
                        }
            };

        const auto surface =
            orbit::path_geometry::
                BuildPathCenterline({
                    .edge =
                        Edge(
                            surfaceEdgeId,
                            network,
                            surfaceStartId,
                            surfaceEndId,
                            orbit::paths::
                                EdgeMode::Direct),
                    .startNode =
                        surfaceStart,
                    .endNode =
                        surfaceEnd,
                    .targetFrame =
                        record->frame,
                    .frames = &frames,
                    .bodies = &bodies
                });

        ORBIT_TEST_CHECK(
            surface.has_value());

        for (const auto& sample :
             surface->samples)
        {
            const auto radial =
                orbit::math::Normalize(
                    sample.position);

            ORBIT_TEST_CHECK(
                orbit::math::Dot(
                    radial,
                    sample.up) >
                0.999);
        }
    }

    return 0;
}
