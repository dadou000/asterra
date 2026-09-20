#include <orbit/studio_session/StudioTerrainServiceStatus.hpp>

#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/studio_session/StudioTerrainRebuildScheduler.hpp>
#include <orbit/terrain_debug/TerrainDebugField.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>

#include <algorithm>

namespace orbit::studio_session
{
namespace
{
[[nodiscard]] const char* ExposedSurfaceName(
    const terrain_material_column::ExposedSurfaceKind surface) noexcept
{
    switch (surface)
    {
    case terrain_material_column::ExposedSurfaceKind::Bedrock:
        return "Bedrock";
    case terrain_material_column::ExposedSurfaceKind::Regolith:
        return "Regolith";
    case terrain_material_column::ExposedSurfaceKind::Soil:
        return "Soil";
    case terrain_material_column::ExposedSurfaceKind::Sand:
        return "Sand";
    case terrain_material_column::ExposedSurfaceKind::Debris:
        return "Debris";
    }

    return "Unknown";
}

[[nodiscard]] std::optional<StudioTerrainViewportRuntimeSnapshot>
SelectRuntime(
    const StudioSession& session,
    const universe::BodyId body,
    const std::string_view preferredViewport)
{
    std::optional<StudioTerrainViewportRuntimeSnapshot> fallback;

    for (const auto& runtime :
         session.TerrainRuntime().Catalog())
    {
        if (runtime.body != body)
        {
            continue;
        }

        if (!preferredViewport.empty() &&
            runtime.viewportId == preferredViewport)
        {
            return runtime;
        }

        if (!fallback.has_value())
        {
            fallback = runtime;
        }
    }

    return fallback;
}

void CaptureExposedSurface(
    const StudioSession& session,
    StudioTerrainServiceStatusSnapshot& result)
{
    if (!result.selectedPhysicalPage.has_value())
    {
        return;
    }

    const auto debugPage =
        session.TerrainDebugPages().Find(
            *result.selectedPhysicalPage);

    if (debugPage == nullptr ||
        !debugPage->Has(
            terrain_debug::TerrainDebugField::ExposedMaterial))
    {
        return;
    }

    const auto view =
        debugPage->View(
            terrain_debug::TerrainDebugField::ExposedMaterial);

    if (view.category.empty() ||
        view.width == 0U ||
        view.height == 0U)
    {
        return;
    }

    const std::size_t x =
        static_cast<std::size_t>(view.width / 2U);
    const std::size_t y =
        static_cast<std::size_t>(view.height / 2U);
    const std::size_t index =
        y * static_cast<std::size_t>(view.width) + x;

    if (index >= view.category.size())
    {
        return;
    }

    const u32 raw = view.category[index];
    if (raw >
        static_cast<u32>(
            terrain_material_column::ExposedSurfaceKind::Debris))
    {
        return;
    }

    const auto surface =
        static_cast<
            terrain_material_column::ExposedSurfaceKind>(
                raw);

    result.exposedSurfaceAvailable = true;
    result.exposedSurfaceName =
        ExposedSurfaceName(surface);
}
} // namespace

std::optional<StudioTerrainServiceStatusSnapshot>
StudioTerrainStatusInspector::Capture(
    StudioSession& session,
    const scene::ObjectId terrainObject,
    const StudioTerrainRebuildScheduler* scheduler,
    const std::string_view preferredViewport)
{
    auto& world = session.World();

    if (!world.HasWorld())
    {
        return std::nullopt;
    }

    const auto body =
        world.Surfaces().
            BodyForTerrainObject(
                terrainObject);

    if (!body.has_value())
    {
        return std::nullopt;
    }

    const auto* services =
        world.Surfaces().
            ServicesForBody(*body);

    if (services == nullptr)
    {
        return std::nullopt;
    }

    StudioTerrainServiceStatusSnapshot result{
        .terrainObject = terrainObject,
        .body = *body,
        .semanticRevision =
            world.Objects().Revision(),
        .surfaceSourceRevision =
            world.Surfaces().SourceRevision(),
        .geologyRevision =
            services->Geology().Revision(),
        .biomeRevision =
            services->Biomes().Revision(),
        .defaultBedrock =
            services->DefaultBedrock(),
        .processes = {
            .streamPowerIterations =
                services->Processes().
                    streamPower.iterations,
            .hydraulicIterations =
                services->Processes().
                    hydraulic.iterations,
            .thermalMaximumIterations =
                services->Processes().
                    thermal.maximumIterations,
            .aeolianIterations =
                services->Processes().
                    aeolian.iterations,
            .glacialIterations =
                services->Processes().
                    glacial.iterations,
            .riverMeanders =
                services->Processes().
                    rivers.enableMeanders,
            .riverMeanderIterations =
                services->Processes().
                    rivers.meanderIterations,
            .riverCutoffs =
                services->Processes().
                    rivers.enableCutoffs,
            .coastalEnabled =
                services->Processes().
                    coastal.enabled,
            .coastalHydrodynamicSteps =
                services->Processes().
                    coastal.hydrodynamicSteps
        },
        .baseBiome =
            services->Biomes().
                BaseBiome().id,
        .baseBiomeName =
            services->Biomes().
                BaseBiome().name,
        .cacheStats =
            services->Cache().Stats(),
        .rebuildSchedulerAttached =
            scheduler != nullptr
    };

    if (const auto* material =
            services->Geology().Find(
                result.defaultBedrock);
        material != nullptr)
    {
        result.defaultBedrockName =
            material->name;
    }

    const auto definitions =
        services->Biomes().Definitions();

    if (!definitions.empty())
    {
        result.optionalBiomeCount =
            static_cast<u32>(
                definitions.size() - 1U);
    }

    const auto runtime =
        SelectRuntime(
            session,
            *body,
            preferredViewport);

    if (runtime.has_value())
    {
        result.terrainSourceRevision =
            runtime->terrainSourceRevision;
        result.selectedPhysicalPage =
            runtime->observerPhysicalPage;
        result.selectedPhysicalLod =
            runtime->physicalPageLevel;
        result.selectedViewport =
            runtime->viewportId;

        CaptureExposedSurface(
            session,
            result);

        if (scheduler != nullptr)
        {
            const auto bodyStatus =
                scheduler->BodyStatus(
                    runtime->planet.id);

            result.regenerationPaused =
                bodyStatus.paused;
            result.rebuildPages =
                bodyStatus.pages;
            result.dirtyPages =
                bodyStatus.dirtyPages;
            result.queuedPages =
                bodyStatus.queuedPages;
            result.buildingPages =
                bodyStatus.buildingPages;
            result.uploadingPages =
                bodyStatus.uploadingPages;
            result.failedPages =
                bodyStatus.failedPages;

            if (const auto pageStatus =
                    scheduler->PageStatus(
                        runtime->
                            observerPhysicalPage);
                pageStatus.has_value())
            {
                result.physicalRevisionFingerprint =
                    pageStatus->
                        revisionFingerprint;
                result.selectedRebuildState =
                    TerrainRebuildStateName(
                        pageStatus->state);
                result.lastRegenerationReason =
                    pageStatus->
                        lastRegenerationReason;
            }
        }
    }

    return result;
}
} // namespace orbit::studio_session
