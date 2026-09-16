#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/frames/FrameGraph.hpp>
#include <orbit/paths/PathNetwork.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>

#include <cmath>
#include <filesystem>
#include <iostream>

#define ORBIT_TEST_CHECK(expression) \
    do \
    { \
        if (!(expression)) \
        { \
            std::cerr << "Path network test failed: " #expression \
                      << " at line " << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

namespace
{
[[nodiscard]] bool Near(
    const orbit::f64 left,
    const orbit::f64 right,
    const orbit::f64 epsilon = 1.0e-9)
{
    return std::abs(left - right) <= epsilon;
}
}

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-path-network-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    auto project =
        orbit::documents::ProjectDocument::Create(
            root,
            "Path Network Test");

    orbit::documents::WorldDatabase world(
        project.StartupWorldPath());
    orbit::schema::SchemaRegistry schemas;
    orbit::paths::RegisterSchemas(schemas);
    orbit::scene::ObjectStore objects(world);
    orbit::commands::CommandService commands(
        objects,
        schemas);
    orbit::paths::PathNetworkService paths(
        objects,
        commands);

    orbit::frames::FrameGraph frames;
    const auto inertial = frames.CreateRoot();

    // Represents a rotating/moving planet frame. The exact motion is not
    // important here: the acceptance property is that the authored local
    // path coordinate remains unchanged while its inertial position evolves.
    const auto bodyFrame =
        frames.CreateFrame(
            inertial,
            [](const orbit::time::SimulationTime time)
            {
                const orbit::f64 seconds =
                    static_cast<orbit::f64>(
                        time.microsecondsFromEpoch) /
                    1'000'000.0;

                return orbit::math::RigidTransformD{
                    .translation = {
                        seconds * 100.0,
                        2'000.0,
                        0.0
                    }
                };
            });

    const auto vehicleFrame =
        frames.CreateFrame(
            bodyFrame,
            [](const orbit::time::SimulationTime time)
            {
                const orbit::f64 seconds =
                    static_cast<orbit::f64>(
                        time.microsecondsFromEpoch) /
                    1'000'000.0;

                return orbit::math::RigidTransformD{
                    .translation = {
                        0.0,
                        seconds * 7.0,
                        3.0
                    }
                };
            });

    const auto network =
        paths.CreateNetwork(
            "Asterra Roads",
            std::nullopt,
            "Content/PathProfiles/Road.orbitpathprofile");

    ORBIT_TEST_CHECK(network.id);
    ORBIT_TEST_CHECK(
        objects.Find(network.object)->type ==
        orbit::paths::kPathNetworkType);

    const orbit::math::Double3 bodyLocal{
        12.0,
        0.0,
        -4.0
    };

    const auto nodeA =
        paths.CreateNode(
            network.id,
            "Road A",
            orbit::paths::FramePointAnchor{
                .frame = bodyFrame,
                .localMeters = bodyLocal
            });

    const auto nodeB =
        paths.CreateNode(
            network.id,
            "Road B",
            orbit::paths::FramePointAnchor{
                .frame = bodyFrame,
                .localMeters = {
                    50.0,
                    2.0,
                    0.0
                }
            });

    const auto storedA =
        paths.FindNode(nodeA.id);
    ORBIT_TEST_CHECK(storedA.has_value());
    ORBIT_TEST_CHECK(
        std::get<orbit::paths::FramePointAnchor>(
            storedA->anchor).
            localMeters == bodyLocal);

    const auto atZero =
        frames.TransformPoint(
            {
                .frame = bodyFrame,
                .localMeters = bodyLocal
            },
            inertial,
            {.microsecondsFromEpoch = 0});

    const auto atTenSeconds =
        frames.TransformPoint(
            {
                .frame = bodyFrame,
                .localMeters = bodyLocal
            },
            inertial,
            {.microsecondsFromEpoch = 10'000'000});

    ORBIT_TEST_CHECK(atZero.has_value());
    ORBIT_TEST_CHECK(atTenSeconds.has_value());
    ORBIT_TEST_CHECK(
        Near(
            atTenSeconds->localMeters.x -
                atZero->localMeters.x,
            1'000.0));
    ORBIT_TEST_CHECK(
        std::get<orbit::paths::FramePointAnchor>(
            paths.FindNode(nodeA.id)->anchor).
            localMeters == bodyLocal);

    // A node authored in a moving vehicle frame likewise keeps its vehicle
    // local coordinates while inertial motion is resolved by FrameGraph.
    const orbit::math::Double3 vehicleLocal{
        1.5,
        0.0,
        0.25
    };

    const auto vehicleNode =
        paths.CreateNode(
            network.id,
            "Vehicle Deck Connector",
            orbit::paths::FramePointAnchor{
                .frame = vehicleFrame,
                .localMeters = vehicleLocal
            });

    const auto vehicleAtZero =
        frames.TransformPoint(
            {
                .frame = vehicleFrame,
                .localMeters = vehicleLocal
            },
            inertial,
            {.microsecondsFromEpoch = 0});
    const auto vehicleAtTen =
        frames.TransformPoint(
            {
                .frame = vehicleFrame,
                .localMeters = vehicleLocal
            },
            inertial,
            {.microsecondsFromEpoch = 10'000'000});

    ORBIT_TEST_CHECK(vehicleAtZero.has_value());
    ORBIT_TEST_CHECK(vehicleAtTen.has_value());
    ORBIT_TEST_CHECK(
        Near(
            vehicleAtTen->localMeters.x -
                vehicleAtZero->localMeters.x,
            1'000.0));
    ORBIT_TEST_CHECK(
        Near(
            vehicleAtTen->localMeters.y -
                vehicleAtZero->localMeters.y,
            70.0));
    ORBIT_TEST_CHECK(
        std::get<orbit::paths::FramePointAnchor>(
            paths.FindNode(vehicleNode.id)->anchor).
            localMeters == vehicleLocal);

    const auto direct =
        paths.ConnectDirect(
            nodeA.id,
            nodeB.id);
    ORBIT_TEST_CHECK(
        direct.mode ==
        orbit::paths::EdgeMode::Direct);

    const orbit::math::Double3 firstHandle{
        10.0,
        0.0,
        0.0
    };
    const orbit::math::Double3 secondHandle{
        -10.0,
        0.0,
        0.0
    };

    const auto bezier =
        paths.ConnectBezier(
            nodeA.id,
            nodeB.id,
            firstHandle,
            secondHandle);

    ORBIT_TEST_CHECK(
        bezier.mode ==
        orbit::paths::EdgeMode::Bezier);

    const orbit::math::Double3 editedStart{
        20.0,
        4.0,
        0.0
    };
    const orbit::math::Double3 editedEnd{
        -15.0,
        -2.0,
        0.0
    };

    paths.SetBezierHandles(
        bezier.id,
        editedStart,
        editedEnd);

    ORBIT_TEST_CHECK(
        paths.FindEdge(bezier.id)->
            startHandleMeters ==
        editedStart);

    commands.Undo();

    ORBIT_TEST_CHECK(
        paths.FindEdge(bezier.id)->
            startHandleMeters ==
        firstHandle);
    ORBIT_TEST_CHECK(
        paths.FindEdge(bezier.id)->
            endHandleMeters ==
        secondHandle);

    commands.Redo();

    ORBIT_TEST_CHECK(
        paths.FindEdge(bezier.id)->
            startHandleMeters ==
        editedStart);
    ORBIT_TEST_CHECK(
        paths.FindEdge(bezier.id)->
            endHandleMeters ==
        editedEnd);

    // Entity/socket anchors are persistent semantic anchors rather than
    // resolved world positions. Runtime entity motion therefore does not
    // mutate the authored path node.
    paths.SetNodeAnchor(
        vehicleNode.id,
        orbit::paths::EntitySocketAnchor{
            .entity = network.object,
            .socket = "deck.road.out",
            .localMeters = vehicleLocal
        });

    const auto socketNode =
        paths.FindNode(vehicleNode.id);
    ORBIT_TEST_CHECK(socketNode.has_value());
    ORBIT_TEST_CHECK(
        std::get<orbit::paths::EntitySocketAnchor>(
            socketNode->anchor).
            socket ==
        "deck.road.out");

    project.Save();
    world.Checkpoint();
    std::filesystem::remove_all(root);
    return 0;
}
