#include <orbit/studio_session/StudioTerrainRebuildScheduler.hpp>

#include <orbit/jobs/JobSystem.hpp>
#include <orbit/procedural_graph/ProceduralGraph.hpp>

#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace
{
using namespace orbit;

[[noreturn]] void Fail(const std::string& message)
{
    std::cerr << "M06 failure: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

void Require(
    const bool condition,
    const std::string& message)
{
    if (!condition)
    {
        Fail(message);
    }
}

terrain::PhysicalTerrainPageAddress MakeAddress(
    const u32 x,
    const u32 y)
{
    return {
        .planet = {
            .high = 0x4F524249544D3036ULL,
            .low = 0x0000000000000001ULL
        },
        .tile = {
            .face = world::CubeFace::PositiveZ,
            .level = 6U,
            .x = x,
            .y = y
        }
    };
}

terrain::TerrainGenerationRevisions InitialRevisions()
{
    return {
        .geology = 10U,
        .climate = 20U,
        .authoring = 30U,
        .biome = 40U,
        .water = 50U,
        .processes = 60U
    };
}

std::size_t ProductIndex(
    const terrain_dependency::TerrainDependencyProduct product)
{
    return static_cast<std::size_t>(
        product);
}

terrain_dependency::TerrainInvalidationRequest GlobalChange(
    const terrain_dependency::TerrainChangeKind kind,
    const world::PlanetId planet)
{
    return {
        .kind = kind,
        .scope = {
            .planet = planet,
            .global = true
        }
    };
}

struct Fixture
{
    explicit Fixture(
        const studio_session::StudioTerrainRebuildConfig config = {
            .editDebounceSeconds = 0.0,
            .maxBuildRequestsPerTick = 2U
        })
        : graph(jobs),
          dependencies(
              graph,
              [this](
                  const terrain::PhysicalTerrainPageAddress&,
                  const terrain_dependency::TerrainDependencyProduct product,
                  const procedural_graph::BuildContext&)
              {
                  const auto index =
                      ProductIndex(product);

                  ++counts[index];

                  while (
                      blockedProduct.load(
                          std::memory_order_acquire) ==
                          static_cast<i32>(index) &&
                      !releaseBlocked.load(
                          std::memory_order_acquire))
                  {
                      std::this_thread::yield();
                  }

                  return std::any(
                      static_cast<u32>(
                          product));
              }),
          scheduler(
              dependencies,
              config)
    {
    }

    [[nodiscard]] u64 Count(
        const terrain_dependency::TerrainDependencyProduct product) const
    {
        return counts[
            ProductIndex(product)].
                load(
                    std::memory_order_acquire);
    }

    void Block(
        const terrain_dependency::TerrainDependencyProduct product)
    {
        releaseBlocked.store(
            false,
            std::memory_order_release);
        blockedProduct.store(
            static_cast<i32>(
                ProductIndex(product)),
            std::memory_order_release);
    }

    void Release()
    {
        releaseBlocked.store(
            true,
            std::memory_order_release);
        blockedProduct.store(
            -1,
            std::memory_order_release);
    }

    jobs::JobSystem jobs{2U};
    procedural_graph::ProceduralGraph graph;

    std::array<
        std::atomic<u64>,
        terrain_dependency::kTerrainDependencyProductCount>
        counts{};

    std::atomic<i32> blockedProduct{-1};
    std::atomic<bool> releaseBlocked{true};

    terrain_dependency::TerrainDependencyGraph
        dependencies;
    studio_session::StudioTerrainRebuildScheduler
        scheduler;
};

void DriveReady(
    Fixture& fixture,
    const terrain::PhysicalTerrainPageAddress& address)
{
    fixture.scheduler.RebuildDirty();

    for (u32 iteration = 0U;
         iteration < 32U;
         ++iteration)
    {
        fixture.jobs.WaitIdle();
        fixture.scheduler.Tick(0.0);

        const auto status =
            fixture.scheduler.PageStatus(
                address);

        Require(
            status.has_value(),
            "Registered page status must exist.");

        if (status->state ==
            studio_session::TerrainRebuildState::Ready)
        {
            return;
        }
    }

    Fail(
        "Terrain rebuild did not converge to Ready.");
}

void TestInitialDirtyBuildConvergesAndReportsProgress()
{
    Fixture fixture;
    const auto address =
        MakeAddress(12U, 14U);

    fixture.scheduler.RegisterPage(
        address,
        InitialRevisions());

    const auto initial =
        fixture.scheduler.PageStatus(
            address);

    Require(
        initial.has_value() &&
        initial->state ==
            studio_session::TerrainRebuildState::Dirty,
        "A newly registered M27 page with unbuilt descendants must start Dirty.");

    DriveReady(
        fixture,
        address);

    const auto ready =
        fixture.scheduler.PageStatus(
            address);

    Require(
        ready.has_value() &&
        ready->state ==
            studio_session::TerrainRebuildState::Ready &&
        ready->completedProducts ==
            ready->totalProducts &&
        ready->totalProducts ==
            terrain_dependency::kTerrainDependencyProductCount,
        "Initial page generation must finish Ready with complete page progress.");

    const auto body =
        fixture.scheduler.BodyStatus(
            address.planet);

    Require(
        body.state ==
            studio_session::TerrainRebuildState::Ready &&
        body.progress == 1.0 &&
        body.readyPages == 1U,
        "Body-level M06 progress must converge with its page.");
}

void TestSliderChangesDebounceAndCoalesce()
{
    Fixture fixture({
        .editDebounceSeconds = 0.20,
        .maxBuildRequestsPerTick = 2U
    });

    const auto address =
        MakeAddress(18U, 19U);

    fixture.scheduler.RegisterPage(
        address,
        InitialRevisions());

    DriveReady(
        fixture,
        address);

    const auto before =
        fixture.dependencies.Revisions(
            address);

    Require(
        before.has_value(),
        "M27 revisions must exist before debounce test.");

    for (u32 edit = 0U;
         edit < 12U;
         ++edit)
    {
        fixture.scheduler.QueueChange(
            GlobalChange(
                terrain_dependency::TerrainChangeKind::TerrainAuthoring,
                address.planet));
    }

    fixture.scheduler.Tick(0.10);

    const auto middle =
        fixture.dependencies.Revisions(
            address);

    Require(
        middle.has_value() &&
        middle->authoring ==
            before->authoring,
        "Debounced slider edits must not increment authority revisions before the debounce expires.");

    fixture.scheduler.Tick(0.10);

    const auto flushed =
        fixture.dependencies.Revisions(
            address);

    Require(
        flushed.has_value() &&
        flushed->authoring ==
            before->authoring + 1U,
        "Repeated slider edits in one debounce window must coalesce to one M27 invalidation.");

    DriveReady(
        fixture,
        address);
}

void TestPauseAndManualRebuildDirty()
{
    Fixture fixture({
        .editDebounceSeconds = 10.0,
        .maxBuildRequestsPerTick = 2U
    });

    const auto address =
        MakeAddress(7U, 9U);

    fixture.scheduler.RegisterPage(
        address,
        InitialRevisions());

    fixture.scheduler.SetPaused(true);
    fixture.scheduler.RebuildDirty();
    fixture.jobs.WaitIdle();
    fixture.scheduler.Tick(0.0);

    Require(
        fixture.Count(
            terrain_dependency::TerrainDependencyProduct::Geology) ==
            0U,
        "Pause Regeneration must prevent dirty work from being submitted.");

    fixture.scheduler.SetPaused(false);
    DriveReady(
        fixture,
        address);

    const auto before =
        fixture.dependencies.Revisions(
            address);

    fixture.scheduler.QueueChange(
        GlobalChange(
            terrain_dependency::TerrainChangeKind::BiomeScatter,
            address.planet));

    fixture.scheduler.RebuildDirty();

    const auto after =
        fixture.dependencies.Revisions(
            address);

    Require(
        before.has_value() &&
        after.has_value() &&
        after->biome ==
            before->biome + 1U,
        "Manual Rebuild Dirty must flush pending edits without waiting for debounce.");

    DriveReady(
        fixture,
        address);
}

void TestM27DescendantsOnly()
{
    Fixture fixture;
    const auto address =
        MakeAddress(22U, 23U);

    fixture.scheduler.RegisterPage(
        address,
        InitialRevisions());

    DriveReady(
        fixture,
        address);

    const u64 geologyBefore =
        fixture.Count(
            terrain_dependency::TerrainDependencyProduct::Geology);
    const u64 processBefore =
        fixture.Count(
            terrain_dependency::TerrainDependencyProduct::TerrainProcesses);
    const u64 surfaceBefore =
        fixture.Count(
            terrain_dependency::TerrainDependencyProduct::SurfaceMaterial);
    const u64 scatterBefore =
        fixture.Count(
            terrain_dependency::TerrainDependencyProduct::Scatter);

    fixture.scheduler.QueueChange(
        GlobalChange(
            terrain_dependency::TerrainChangeKind::BiomeScatter,
            address.planet));

    DriveReady(
        fixture,
        address);

    Require(
        fixture.Count(
            terrain_dependency::TerrainDependencyProduct::Scatter) ==
            scatterBefore + 1U,
        "Biome scatter authoring must rebuild Scatter.");

    Require(
        fixture.Count(
            terrain_dependency::TerrainDependencyProduct::Geology) ==
            geologyBefore &&
        fixture.Count(
            terrain_dependency::TerrainDependencyProduct::TerrainProcesses) ==
            processBefore &&
        fixture.Count(
            terrain_dependency::TerrainDependencyProduct::SurfaceMaterial) ==
            surfaceBefore,
        "M06 must request only the M27 descendants dirtied by the authority change.");
}

void TestCpuAndGpuBuildStates()
{
    Fixture fixture;
    const auto address =
        MakeAddress(28U, 29U);

    fixture.scheduler.RegisterPage(
        address,
        InitialRevisions());

    DriveReady(
        fixture,
        address);

    fixture.Block(
        terrain_dependency::TerrainDependencyProduct::SurfaceMaterial);

    fixture.scheduler.QueueChange(
        GlobalChange(
            terrain_dependency::TerrainChangeKind::BiomeSurfaceMaterial,
            address.planet));
    fixture.scheduler.Tick(0.0);

    auto status =
        fixture.scheduler.PageStatus(
            address);

    Require(
        status.has_value() &&
        status->state ==
            studio_session::TerrainRebuildState::BuildingCpu,
        "CPU M27 products must report Building CPU.");

    fixture.Release();
    DriveReady(
        fixture,
        address);

    fixture.Block(
        terrain_dependency::TerrainDependencyProduct::Scatter);

    fixture.scheduler.QueueChange(
        GlobalChange(
            terrain_dependency::TerrainChangeKind::BiomeScatter,
            address.planet));
    fixture.scheduler.Tick(0.0);

    status =
        fixture.scheduler.PageStatus(
            address);

    Require(
        status.has_value() &&
        status->state ==
            studio_session::TerrainRebuildState::BuildingGpu,
        "GPU M27 products must report Building GPU.");

    fixture.Release();
    DriveReady(
        fixture,
        address);
}

void TestBoundedRequestBudgetAcrossPages()
{
    Fixture fixture({
        .editDebounceSeconds = 0.0,
        .maxBuildRequestsPerTick = 1U
    });

    const auto first =
        MakeAddress(31U, 32U);
    const auto second =
        MakeAddress(32U, 32U);

    fixture.scheduler.RegisterPage(
        first,
        InitialRevisions());
    fixture.scheduler.RegisterPage(
        second,
        InitialRevisions());

    fixture.scheduler.RebuildDirty();

    const auto a =
        fixture.scheduler.PageStatus(
            first);
    const auto b =
        fixture.scheduler.PageStatus(
            second);

    Require(
        a.has_value() &&
        b.has_value(),
        "Budget test pages must be registered.");

    const u32 untouched =
        static_cast<u32>(
            a->state ==
                studio_session::TerrainRebuildState::Dirty) +
        static_cast<u32>(
            b->state ==
                studio_session::TerrainRebuildState::Dirty);

    Require(
        untouched >= 1U,
        "A one-request frame budget must leave at least one dirty page unscheduled.");
}

void TestStaleUploadCannotCommitOverNewRevision()
{
    Fixture fixture;
    const auto address =
        MakeAddress(40U, 41U);

    fixture.scheduler.RegisterPage(
        address,
        InitialRevisions());

    DriveReady(
        fixture,
        address);

    const auto uploadRevision =
        fixture.scheduler.BeginUpload(
            address);

    Require(
        uploadRevision.has_value(),
        "Ready page must be eligible for upload tracking.");

    fixture.scheduler.QueueChange(
        GlobalChange(
            terrain_dependency::TerrainChangeKind::TerrainAuthoring,
            address.planet));
    fixture.scheduler.Tick(0.0);

    Require(
        !fixture.scheduler.CompleteUpload(
            address,
            *uploadRevision,
            true),
        "Upload completion from an obsolete authority revision must be rejected.");

    const auto status =
        fixture.scheduler.PageStatus(
            address);

    Require(
        status.has_value() &&
        status->staleRejected > 0U,
        "Stale/replaced completion must be visible in page diagnostics.");

    DriveReady(
        fixture,
        address);

    const auto currentUpload =
        fixture.scheduler.BeginUpload(
            address);

    Require(
        currentUpload.has_value() &&
        fixture.scheduler.CompleteUpload(
            address,
            *currentUpload,
            true),
        "Current-revision upload completion must be accepted.");
}
} // namespace

int main()
{
    TestInitialDirtyBuildConvergesAndReportsProgress();
    TestSliderChangesDebounceAndCoalesce();
    TestPauseAndManualRebuildDirty();
    TestM27DescendantsOnly();
    TestCpuAndGpuBuildStates();
    TestBoundedRequestBudgetAcrossPages();
    TestStaleUploadCannotCommitOverNewRevision();

    std::cout
        << "Orbit V0.0.5 M06 terrain rebuild scheduler tests passed.\n";
    return EXIT_SUCCESS;
}
