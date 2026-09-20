#include <orbit/studio_session/StudioTerrainValidationScenario.hpp>

#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/studio_session/StudioTerrainAuthoringInvalidation.hpp>
#include <orbit/terrain_biome/BiomeService.hpp>
#include <orbit/terrain_debug/TerrainDebugField.hpp>
#include <orbit/world/Planet.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace orbit::studio_session
{
namespace
{
void AddStep(
    StudioTerrainValidationScenarioReport& report,
    std::string name,
    const bool passed,
    std::string diagnostic = {})
{
    report.steps.push_back({
        .name = std::move(name),
        .passed = passed,
        .diagnostic = std::move(diagnostic)
    });
}

void Fail(
    StudioTerrainValidationScenarioReport& report,
    const std::string_view stage,
    const std::string_view diagnostic)
{
    report.success = false;
    report.failureStage = std::string(stage);
    report.diagnostic = std::string(diagnostic);
    AddStep(
        report,
        std::string(stage),
        false,
        std::string(diagnostic));
}

[[nodiscard]] scene::ObjectId OnlyChildOfType(
    StudioSession& studio,
    const scene::ObjectId parent,
    const schema::TypeId type)
{
    scene::ObjectId result{};
    u32 count = 0U;

    for (const auto& child :
         studio.World().Objects().Children(parent))
    {
        if (child.type != type)
        {
            continue;
        }

        result = child.id;
        ++count;
    }

    if (count != 1U)
    {
        throw std::runtime_error(
            "M15 expected exactly one authored child of the requested type.");
    }

    return result;
}

[[nodiscard]] std::shared_ptr<
    const StudioTerrainPhysicalPageSnapshot>
DrivePageReady(
    StudioSession& session,
    const terrain::PhysicalTerrainPageAddress& address)
{
    constexpr u32 kMaximumIterations = 40'000U;

    for (u32 iteration = 0U;
         iteration < kMaximumIterations;
         ++iteration)
    {
        static_cast<void>(
            session.Tick(false));

        session.TerrainPhysicalPages().
            RebuildDirty();

        const auto status =
            session.TerrainPhysicalPages().
                PageStatus(address);

        const auto page =
            session.TerrainPhysicalPages().
                Find(address);

        if (status.has_value() &&
            status->state ==
                TerrainRebuildState::Ready &&
            page != nullptr)
        {
            return page;
        }

        std::this_thread::yield();
    }

    return nullptr;
}

[[nodiscard]] math::Double3 ValidationDirection()
{
    return math::Normalize(
        math::Double3{
            0.31,
            0.22,
            -0.924
        });
}
} // namespace

StudioTerrainValidationScenarioReport
RunStudioTerrainValidationScenario(
    const std::filesystem::path& rootDirectory,
    const std::string_view viewportId)
{
    StudioTerrainValidationScenarioReport report{};
    report.projectRoot = rootDirectory;

    try
    {
        if (rootDirectory.empty())
        {
            Fail(
                report,
                "preflight",
                "M15 requires a non-empty validation project root.");
            return report;
        }

        if (std::filesystem::exists(rootDirectory))
        {
            Fail(
                report,
                "preflight",
                "M15 validation project root already exists; refusing to overwrite it.");
            return report;
        }

        StudioWorkspace workspace;
        workspace.CreateProject(
            rootDirectory,
            "M15 Terrain Validation");

        report.projectManifest =
            workspace.Project().ManifestPath();

        AddStep(
            report,
            "create-project",
            true,
            "Created an isolated project through StudioWorkspace.");

        auto& studio = workspace.Session();

        const auto validationWorld =
            studio.CreateWorld(
                "M15Validation",
                "M15 Terrain Validation");

        studio.OpenWorld(
            validationWorld.relativePath);

        report.worldPath =
            validationWorld.relativePath;

        AddStep(
            report,
            "create-open-world",
            studio.World().HasWorld(),
            "Created and opened a world through StudioSession lifecycle services.");

        studio.Viewports().Register(
            std::string(viewportId),
            ViewportMode::Perspective,
            true);

        const auto worldObject =
            studio.World().Commands().
                CreateObject(
                    world_model::kWorldType,
                    "World");

        const scene::ObjectId selectedWorld[] = {
            worldObject
        };

        studio.World().Selection().Set(
            selectedWorld);

        studio.World().CommandRegistry().
            Invoke(
                editor_model::authoring_commands::
                    kCreateRockyPlanet,
                {
                    {
                        "name",
                        std::string(
                            "M15 Rocky Planet")
                    },
                    {
                        "radiusMeters",
                        6'000'000.0
                    },
                    {
                        "massKg",
                        5.0e24
                    }
                });

        if (studio.World().Selection().
                Ordered().size() != 1U)
        {
            Fail(
                report,
                "create-rocky-planet",
                "Create Rocky Planet did not leave exactly one body selected.");
            return report;
        }

        report.semanticBody =
            studio.World().Selection().
                Ordered().front();

        report.terrainObject =
            OnlyChildOfType(
                studio,
                report.semanticBody,
                world_model::
                    kTerrainSurfaceType);

        static_cast<void>(
            studio.Tick(false));

        auto runtime =
            studio.TerrainRuntime().
                Capture(viewportId);

        if (!runtime.has_value())
        {
            Fail(
                report,
                "open-production-perspective",
                "Perspective viewport did not bind the production terrain runtime.");
            return report;
        }

        AddStep(
            report,
            "create-rocky-planet",
            true,
            "Created the body through the shared Create Rocky Planet command.");
        AddStep(
            report,
            "open-production-perspective",
            true,
            "Perspective viewport is bound to StudioTerrainRuntimeBridge.");

        const auto testDirection =
            ValidationDirection();

        const world::WorldPosition observer{
            .meters =
                testDirection *
                (runtime->planet.radiusMeters +
                 2'000.0)
        };

        if (!studio.TerrainRuntime().
                SetObserver(
                    viewportId,
                    observer))
        {
            Fail(
                report,
                "focus-test-region",
                "Could not move the production terrain observer to the deterministic M15 region.");
            return report;
        }

        runtime =
            studio.TerrainRuntime().
                Capture(viewportId);

        if (!runtime.has_value())
        {
            Fail(
                report,
                "focus-test-region",
                "Terrain runtime vanished after focusing the M15 test region.");
            return report;
        }

        AddStep(
            report,
            "focus-test-region",
            true,
            "Focused a deterministic body-space region through the production observer seam.");

        editor_model::SurfaceAuthoringModel surface(
            studio.World().Objects(),
            studio.World().Commands(),
            studio.World().Selection());

        // Keep the deterministic acceptance scenario fast while still driving
        // the authored process record. A later step changes one process setting
        // and re-enables one bounded process through the same public model.
        auto processSettings =
            surface.ProcessSettings(
                report.terrainObject);

        processSettings.streamPowerEnabled = false;
        processSettings.hydraulicEnabled = false;
        processSettings.thermalEnabled = false;
        processSettings.aeolianEnabled = false;
        processSettings.glacialEnabled = false;
        processSettings.riversEnabled = false;
        processSettings.coastal.enabled = false;

        surface.SetProcessSettings(
            report.terrainObject,
            processSettings);

        auto relief =
            surface.Relief(
                report.terrainObject);

        relief.macroAmplitudeMeters +=
            137.0;
        relief.detailAmplitudeMeters +=
            19.0;

        surface.SetRelief(
            report.terrainObject,
            relief);

        static_cast<void>(
            studio.Tick(false));

        runtime =
            studio.TerrainRuntime().
                Capture(viewportId);

        if (!runtime.has_value())
        {
            Fail(
                report,
                "edit-base-relief",
                "Terrain runtime did not rebind after the authored relief edit.");
            return report;
        }

        AddStep(
            report,
            "edit-base-relief",
            true,
            "Changed authored relief through SurfaceAuthoringModel and refreshed composition.");

        const auto frame =
            world::MakeSurfaceFrame(
                testDirection);

        const std::vector<math::Double3>
            canyonPoints{
                world::DirectionAtSurfaceOffset(
                    runtime->planet,
                    frame,
                    {-1'800.0, -600.0}),
                world::DirectionAtSurfaceOffset(
                    runtime->planet,
                    frame,
                    {0.0, 0.0}),
                world::DirectionAtSurfaceOffset(
                    runtime->planet,
                    frame,
                    {2'000.0, 850.0})
            };

        report.canyonConstraint =
            surface.AddCanyonSpline(
                report.terrainObject,
                canyonPoints,
                320.0,
                480.0,
                90.0);

        const auto canyonInvalidations =
            BuildTerrainAuthoringInvalidations(
                runtime->planet,
                canyonPoints,
                800.0,
                runtime->physicalPageLevel,
                3U,
                terrain_dependency::
                    TerrainChangeKind::
                        TerrainAuthoring);

        studio.QueueTerrainInvalidations(
            canyonInvalidations);

        AddStep(
            report,
            "author-canyon",
            report.canyonConstraint.IsValid() &&
                !canyonInvalidations.empty(),
            "Committed a canyon spline and bounded M27 invalidations through the same services as the viewport tool.");

        report.biomeObject =
            surface.AddBiome(
                report.terrainObject,
                "M15 Validation Biome");

        report.biomeMask =
            surface.PaintBiomeMask(
                report.biomeObject,
                terrain_biome::
                    BiomeAuthoredWeightOperation::
                        Replace,
                testDirection,
                500.0,
                6'000.0,
                0.8,
                1.0);

        const math::Double3 biomePoint =
            testDirection;

        const auto biomeInvalidations =
            BuildTerrainAuthoringInvalidations(
                runtime->planet,
                std::span{
                    &biomePoint,
                    std::size_t{1U}},
                6'000.0,
                runtime->physicalPageLevel,
                0U,
                terrain_dependency::
                    TerrainChangeKind::
                        BiomePlacement);

        studio.QueueTerrainInvalidations(
            biomeInvalidations);

        AddStep(
            report,
            "paint-biome",
            report.biomeObject.IsValid() &&
                report.biomeMask.IsValid() &&
                !biomeInvalidations.empty(),
            "Painted a Replace biome mask and queued only biome-placement descendants.");

        processSettings =
            surface.ProcessSettings(
                report.terrainObject);

        processSettings.streamPowerEnabled = true;
        processSettings.streamPower.iterations = 1U;

        surface.SetProcessSettings(
            report.terrainObject,
            processSettings);

        studio.QueueTerrainInvalidation({
            .kind =
                terrain_dependency::
                    TerrainChangeKind::
                        ProcessSettings,
            .scope = {
                .planet =
                    runtime->planet.id,
                .global = true
            }
        });

        AddStep(
            report,
            "edit-process-setting",
            true,
            "Enabled one-iteration stream-power erosion through the authored process record.");

        report.queuedInvalidations =
            static_cast<u32>(
                studio.PendingTerrainInvalidations().
                    size());

        static_cast<void>(
            studio.Tick(false));

        runtime =
            studio.TerrainRuntime().
                Capture(viewportId);

        if (!runtime.has_value())
        {
            Fail(
                report,
                "wait-current-revision",
                "Production terrain runtime vanished after M15 edits.");
            return report;
        }

        report.comparisonPage =
            runtime->observerPhysicalPage;

        report.terrainSourceRevisionAfterEdits =
            runtime->terrainSourceRevision;

        report.semanticRevisionAfterEdits =
            surface.Counts(
                report.terrainObject).
                semanticRevision;

        const auto readyPage =
            DrivePageReady(
                studio,
                report.comparisonPage);

        if (readyPage == nullptr)
        {
            Fail(
                report,
                "wait-current-revision",
                "Edited comparison page did not reach Ready.");
            return report;
        }

        // Refresh the runtime-facing diagnostics after the page reaches Ready.
        // A single focus transition may still retain completing pages from the
        // previous five-page observer neighborhood; the M16 gate bounds that
        // transition separately from steady-state renderer cache reuse.
        static_cast<void>(
            studio.Tick(false));

        AddStep(
            report,
            "wait-current-revision",
            true,
            "Newest edited physical page reached Ready through M06/M12.");

        const auto debugPage =
            studio.TerrainDebugPages().
                Find(
                    report.comparisonPage);

        if (debugPage == nullptr)
        {
            Fail(
                report,
                "inspect-m29",
                "Ready physical page was not published through M29.");
            return report;
        }

        for (const auto& descriptor :
             terrain_debug::FieldCatalog())
        {
            if (debugPage->Has(
                    descriptor.field))
            {
                ++report.debugFieldsAvailable;
            }
        }

        report.debugPhysicalLodAvailable =
            debugPage->Has(
                terrain_debug::
                    TerrainDebugField::
                        PhysicalLod);

        report.debugBiomeWeightsAvailable =
            debugPage->Has(
                terrain_debug::
                    TerrainDebugField::
                        BiomeWeights);

        runtime =
            studio.TerrainRuntime().
                Capture(viewportId);

        if (!runtime.has_value())
        {
            Fail(
                report,
                "inspect-telemetry",
                "Terrain runtime vanished while capturing M26 telemetry.");
            return report;
        }

        report.cacheStats =
            runtime->cacheStats;

        report.performance =
            studio.TerrainPerformance().
                Capture(
                    studio,
                    viewportId);

        if (!report.debugPhysicalLodAvailable ||
            !report.debugBiomeWeightsAvailable ||
            report.debugFieldsAvailable == 0U)
        {
            Fail(
                report,
                "inspect-m29",
                "M29 page did not expose the required physical-LOD and biome-weight diagnostic fields.");
            return report;
        }

        AddStep(
            report,
            "inspect-m29",
            true,
            "Captured live M29 fields from the same Ready physical page.");

        AddStep(
            report,
            "inspect-cache-telemetry",
            true,
            "Captured M26 cache counters; GPU residency remains renderer-owned in this headless scenario.");

        report.roundTrip =
            VerifyStudioTerrainRoundTrip(
                workspace,
                viewportId);

        if (!report.roundTrip.success)
        {
            Fail(
                report,
                "save-reopen-verify",
                report.roundTrip.diagnostic.empty()
                    ? "M14 round-trip verification failed."
                    : report.roundTrip.diagnostic);
            return report;
        }

        AddStep(
            report,
            "save-reopen-verify",
            true,
            "M14 verified authored identity, fresh derived state and regenerated physical equivalence.");

        report.success = true;
        report.failureStage.clear();
        report.diagnostic =
            "M15 Studio terrain validation scenario passed.";
        return report;
    }
    catch (const std::exception& exception)
    {
        Fail(
            report,
            "exception",
            exception.what());
        return report;
    }
}
} // namespace orbit::studio_session
