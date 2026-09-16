#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/paths/Routing.hpp>
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
            std::cerr << "Routing preparation test failed: " #expression \
                      << " at line " << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-routing-preparation-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Routing Preparation Test");
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
        const auto rootFrame =
            frames.CreateRoot();
        const auto movingFrame =
            frames.CreateFrame(
                rootFrame,
                [](const orbit::time::SimulationTime time)
                {
                    const orbit::f64 seconds =
                        static_cast<orbit::f64>(
                            time.microsecondsFromEpoch) /
                        1'000'000.0;

                    return orbit::math::RigidTransformD{
                        .translation = {
                            100.0 + seconds * 2.0,
                            0.0,
                            0.0
                        }
                    };
                });

        orbit::universe::BodyRegistry bodies(frames);

        const auto network =
            paths.CreateNetwork("Cross Frame Route");
        const auto start =
            paths.CreateNode(
                network.id,
                "Start",
                orbit::paths::FramePointAnchor{
                    .frame = rootFrame,
                    .localMeters = {0.0, 0.0, 0.0}
                });
        const auto end =
            paths.CreateNode(
                network.id,
                "Moving End",
                orbit::paths::FramePointAnchor{
                    .frame = movingFrame,
                    .localMeters = {10.0, 0.0, 0.0}
                });
        const auto edge =
            paths.ConnectRouted(
                start.id,
                end.id);

        orbit::paths::RouteDependencyRevisions dependencies{
            .terrain = 11,
            .profile = 12,
            .costFields = 13,
            .endpoints = 14
        };

        orbit::paths::PathProfile profile{
            .name = "Road",
            .maximumGrade = 1.0
        };

        std::string failure;
        const auto prepared =
            orbit::paths::PrepareRouteSolveRequest(
                paths,
                edge.id,
                rootFrame,
                {.microsecondsFromEpoch = 5'000'000},
                frames,
                bodies,
                profile,
                [](const orbit::math::Double3& point)
                {
                    return orbit::paths::RouteCostSample{
                        .position = point
                    };
                },
                dependencies,
                5.0,
                50.0,
                {},
                &failure);

        ORBIT_TEST_CHECK(prepared.has_value());
        ORBIT_TEST_CHECK(failure.empty());
        ORBIT_TEST_CHECK(
            std::abs(prepared->start.x) < 1.0e-9);
        ORBIT_TEST_CHECK(
            std::abs(prepared->end.x - 120.0) < 1.0e-9);
        ORBIT_TEST_CHECK(
            prepared->dependencyRevision ==
            orbit::paths::RouteDependencySignature(dependencies));

        const auto solved =
            orbit::paths::SolveRoute(*prepared);
        ORBIT_TEST_CHECK(solved.has_value());
        ORBIT_TEST_CHECK(
            solved->dependencyRevision ==
            prepared->dependencyRevision);

        world.Checkpoint();
    }

    std::filesystem::remove_all(root);
    return 0;
}
