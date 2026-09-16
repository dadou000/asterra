#include <orbit/procedural_graph/ProceduralGraph.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <iostream>

#define ORBIT_TEST_CHECK(expression) \
    do \
    { \
        if (!(expression)) \
        { \
            std::cerr << "ProceduralGraph test failed: " #expression \
                      << " at line " << __LINE__ << '\\n'; \
            return __LINE__; \
        } \
    } while (false)

int main()
{
    orbit::jobs::JobSystem jobs(2);
    orbit::procedural_graph::
        ProceduralGraph graph(jobs);

    const auto axialTilt =
        graph.AddSource(
            "Axial tilt",
            1);

    const auto tectonics =
        graph.AddSource(
            "Tectonics",
            1);

    std::atomic<int>
        climateBuildCount{0};
    std::atomic<int>
        biomeBuildCount{0};
    std::atomic<int>
        terrainBuildCount{0};

    const auto climate =
        graph.AddDerived(
            "Climate",
            {axialTilt},
            orbit::procedural_graph::
                ExecutionBackend::Cpu,
            [&climateBuildCount](
                const auto& context)
            {
                ++climateBuildCount;
                return std::any(
                    std::string(
                        "climate-") +
                    std::to_string(
                        context.
                            inputRevisionHash));
            });

    const auto biomes =
        graph.AddDerived(
            "Biomes",
            {climate},
            orbit::procedural_graph::
                ExecutionBackend::Cpu,
            [&biomeBuildCount](
                const auto&)
            {
                ++biomeBuildCount;
                return std::any(42);
            });

    const auto terrain =
        graph.AddDerived(
            "Terrain",
            {tectonics},
            orbit::procedural_graph::
                ExecutionBackend::Gpu,
            [&terrainBuildCount](
                const auto&)
            {
                ++terrainBuildCount;
                return std::any(7);
            });

    ORBIT_TEST_CHECK(graph.BuildBlocking(biomes));
    ORBIT_TEST_CHECK(graph.BuildBlocking(terrain));

    ORBIT_TEST_CHECK(climateBuildCount == 1);
    ORBIT_TEST_CHECK(biomeBuildCount == 1);
    ORBIT_TEST_CHECK(terrainBuildCount == 1);

    const auto terrainBefore =
        graph.Status(terrain);
    ORBIT_TEST_CHECK(terrainBefore.has_value());

    // Axial tilt affects climate and biomes, but not the independent
    // tectonics -> terrain chain.
    graph.SetSourceRevision(
        axialTilt,
        2);

    ORBIT_TEST_CHECK(graph.BuildBlocking(biomes));

    ORBIT_TEST_CHECK(climateBuildCount == 2);
    ORBIT_TEST_CHECK(biomeBuildCount == 2);
    ORBIT_TEST_CHECK(terrainBuildCount == 1);

    const auto terrainAfter =
        graph.Status(terrain);

    ORBIT_TEST_CHECK(terrainAfter.has_value());
    ORBIT_TEST_CHECK(
        terrainAfter->committedRevision ==
        terrainBefore->committedRevision);

    // Stale completion test. Invalidate a node while its old job is
    // running; that old product must be discarded and a new generation
    // must build before the node becomes clean.
    std::atomic<bool> releaseOldBuild{false};
    std::atomic<int> staleBuildCount{0};

    const auto source =
        graph.AddSource(
            "Stale source",
            1);

    const auto derived =
        graph.AddDerived(
            "Stale derived",
            {source},
            orbit::procedural_graph::
                ExecutionBackend::Cpu,
            [&releaseOldBuild,
             &staleBuildCount](
                const auto& context)
            {
                const int buildNumber =
                    ++staleBuildCount;

                if (buildNumber == 1)
                {
                    while (!releaseOldBuild.
                               load(
                                   std::memory_order_acquire))
                    {
                        std::this_thread::yield();
                    }
                }

                return std::any(
                    static_cast<int>(
                        context.generation));
            });

    graph.RequestBuild(derived);
    graph.Poll();

    const auto building =
        graph.Status(derived);
    ORBIT_TEST_CHECK(building.has_value());
    ORBIT_TEST_CHECK(
        building->state ==
        orbit::procedural_graph::
            NodeState::Building);

    graph.SetSourceRevision(
        source,
        2);

    releaseOldBuild.store(
        true,
        std::memory_order_release);

    jobs.WaitIdle();
    graph.Poll();

    const auto afterStale =
        graph.Status(derived);

    ORBIT_TEST_CHECK(afterStale.has_value());
    ORBIT_TEST_CHECK(
        afterStale->state ==
        orbit::procedural_graph::
            NodeState::Dirty);

    ORBIT_TEST_CHECK(graph.BuildBlocking(derived));
    ORBIT_TEST_CHECK(staleBuildCount == 2);

    const int* product =
        graph.Product<int>(derived);

    ORBIT_TEST_CHECK(product != nullptr);
    ORBIT_TEST_CHECK(
        *product ==
        static_cast<int>(
            graph.Status(derived)->
                requestedGeneration));

    return 0;
}
