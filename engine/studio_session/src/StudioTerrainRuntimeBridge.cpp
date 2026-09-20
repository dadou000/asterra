#include <orbit/studio_session/StudioTerrainRuntimeBridge.hpp>

#include <orbit/math/Vector.hpp>
#include <orbit/terrain_stream/TerrainMorphRefresh.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace orbit::studio_session
{
namespace
{
[[nodiscard]] bool ClipmapConfigIsValid(
    const terrain_view::ClipmapConfig& config) noexcept
{
    return
        config.levelCount > 0U &&
        config.gridResolution >= 9U &&
        (config.gridResolution % 2U) == 1U &&
        ((config.gridResolution - 1U) % 4U) == 0U &&
        config.baseSpacingMeters > 0.0 &&
        std::isfinite(config.baseSpacingMeters) &&
        std::abs(config.levelScale - 2.0) <=
            1.0e-12 &&
        config.overlapCells > 0U &&
        config.overlapCells <
            (config.gridResolution - 1U) / 2U;
}

[[nodiscard]] world::WorldPosition DefaultObserver(
    const world::PlanetDefinition& planet,
    const f64 altitudeMeters)
{
    return {
        .meters = {
            planet.radiusMeters +
                altitudeMeters,
            0.0,
            0.0
        }
    };
}

[[nodiscard]] bool ObserverValid(
    const world::PlanetDefinition& planet,
    const world::WorldPosition& observer) noexcept
{
    const f64 radius =
        math::Length(observer.meters);

    return
        std::isfinite(radius) &&
        radius > planet.radiusMeters;
}

[[nodiscard]] terrain::PhysicalTerrainPageAddress
ObserverPhysicalPage(
    const world::PlanetDefinition& planet,
    const world::WorldPosition& observer,
    const u8 level)
{
    return {
        .planet = planet.id,
        .tile =
            world::TileForDirection(
                math::Normalize(
                    observer.meters),
                level)
    };
}
} // namespace

struct StudioTerrainRuntimeBridge::RuntimeState
{
    RuntimeState(
        std::string viewport,
        const u64 worldGenerationIn,
        const u64 universeGenerationIn,
        const u64 runtimeGenerationIn,
        const scene::ObjectId semanticBodyIn,
        const universe::BodyId bodyIn,
        const scene::ObjectId terrainObjectIn,
        const world::PlanetDefinition planetIn,
        const u64 terrainSourceRevisionIn,
        const u64 surfaceSourceRevisionIn,
        const terrain_gpu::PersistentGpuTerrainCacheStats cacheStatsIn,
        const terrain_view::ClipmapConfig baseClipmapIn)
        : viewportId(std::move(viewport)),
          worldGeneration(worldGenerationIn),
          universeGeneration(universeGenerationIn),
          runtimeGeneration(runtimeGenerationIn),
          semanticBody(semanticBodyIn),
          body(bodyIn),
          terrainObject(terrainObjectIn),
          planet(planetIn),
          terrainSourceRevision(terrainSourceRevisionIn),
          surfaceSourceRevision(surfaceSourceRevisionIn),
          cacheStats(cacheStatsIn),
          baseClipmap(baseClipmapIn),
          activeClipmap(baseClipmapIn),
          tracker(planetIn, baseClipmapIn),
          residency(baseClipmapIn)
    {
    }

    std::string viewportId;

    u64 worldGeneration{0U};
    u64 universeGeneration{0U};
    u64 runtimeGeneration{0U};

    scene::ObjectId semanticBody{};
    universe::BodyId body{};
    scene::ObjectId terrainObject{};

    world::PlanetDefinition planet{};
    world::WorldPosition observer{};

    terrain::PhysicalTerrainPageAddress
        observerPhysicalPage{};

    u64 terrainSourceRevision{0U};
    u64 surfaceSourceRevision{0U};

    terrain_gpu::PersistentGpuTerrainCacheStats
        cacheStats{};

    terrain_view::ClipmapConfig baseClipmap{};
    terrain_view::ClipmapConfig activeClipmap{};
    u32 adaptiveCoverageTier{0U};

    terrain_view::ClipmapLayout layout{};
    terrain_view::ClipmapTracker tracker;
    terrain_stream::ToroidalResidency residency;
    terrain_view::ClipmapMotionUpdate motion{};
    terrain_stream::ResidencyUpdate residencyUpdate{};

    std::vector<terrain_stream::TerrainSampleRequest>
        requests;
};

bool StudioTerrainRuntimeConfig::IsValid() const noexcept
{
    return
        ClipmapConfigIsValid(clipmap) &&
        physicalPageLevel <= 30U &&
        std::isfinite(
            defaultObserverAltitudeMeters) &&
        defaultObserverAltitudeMeters > 0.0 &&
        std::isfinite(
            adaptiveCoverage.
                altitudeToHalfExtentScale) &&
        adaptiveCoverage.
                altitudeToHalfExtentScale > 0.0 &&
        std::isfinite(
            adaptiveCoverage.
                growThreshold) &&
        std::isfinite(
            adaptiveCoverage.
                shrinkThreshold) &&
        adaptiveCoverage.
                shrinkThreshold > 0.0 &&
        adaptiveCoverage.
                growThreshold >
            adaptiveCoverage.
                shrinkThreshold &&
        adaptiveCoverage.
                growThreshold <= 1.0;
}

StudioTerrainRuntimeBridge::
StudioTerrainRuntimeBridge(
    editor_session::EditorWorldSession& world,
    ViewportTargetRegistry& viewports,
    terrain_debug::TerrainDebugLivePages& debugPages,
    StudioTerrainRuntimeConfig config)
    : world_(&world),
      viewports_(&viewports),
      debugPages_(&debugPages),
      config_(std::move(config))
{
    if (!config_.IsValid())
    {
        throw std::invalid_argument(
            "Studio terrain runtime bridge configuration is invalid.");
    }
}

bool StudioTerrainRuntimeBridge::Refresh()
{
    if (world_ == nullptr ||
        viewports_ == nullptr)
    {
        return false;
    }

    if (!world_->HasWorld())
    {
        const bool changed =
            !runtimes_.empty();

        Clear();

        observedWorldGeneration_ =
            world_->Generation();
        observedUniverseGeneration_ =
            world_->UniverseGeneration();

        return changed;
    }

    const u64 worldGeneration =
        world_->Generation();

    const u64 universeGeneration =
        world_->UniverseGeneration();

    const bool worldGenerationChanged =
        worldGeneration !=
            observedWorldGeneration_;

    const bool universeGenerationChanged =
        universeGeneration !=
            observedUniverseGeneration_;

    const bool generationChanged =
        worldGenerationChanged ||
        universeGenerationChanged;

    bool changed = false;

    if (generationChanged)
    {
        if (worldGenerationChanged)
        {
            observerHistory_.clear();
        }
        else
        {
            for (const auto& [id, runtime] :
                 runtimes_)
            {
                observerHistory_.insert_or_assign(
                    id,
                    ObserverHistory{
                        .semanticBody =
                            runtime->semanticBody,
                        .planet =
                            runtime->planet.id,
                        .observer =
                            runtime->observer
                    });
            }
        }

        changed =
            !runtimes_.empty();

        runtimes_.clear();

        observedWorldGeneration_ =
            worldGeneration;
        observedUniverseGeneration_ =
            universeGeneration;
    }

    const auto targets =
        viewports_->Catalog();

    std::map<
        std::string,
        bool,
        std::less<>>
        seen;

    for (const auto& target :
         targets)
    {
        seen.emplace(
            target.id,
            true);

        if (!target.target.has_value())
        {
            const auto existing =
                runtimes_.find(
                    target.id);

            if (existing !=
                runtimes_.end())
            {
                observerHistory_.
                    insert_or_assign(
                        target.id,
                        ObserverHistory{
                            .semanticBody =
                                existing->second->
                                    semanticBody,
                            .planet =
                                existing->second->
                                    planet.id,
                            .observer =
                                existing->second->
                                    observer
                        });

                runtimes_.erase(
                    existing);

                changed = true;
            }

            continue;
        }

        const auto body =
            target.target->body;

        const auto* capability =
            world_->Surfaces().
                Registry().
                FindTerrainSurface(
                    body);

        const auto planet =
            world_->Surfaces().
                Registry().
                SphericalPlanetDefinition(
                    body);

        const auto terrainObject =
            world_->Surfaces().
                TerrainObjectForBody(
                    body);

        const auto* services =
            world_->Surfaces().
                ServicesForBody(
                    body);

        if (capability == nullptr ||
            capability->terrain == nullptr ||
            !planet.has_value() ||
            !planet->id.IsValid() ||
            !terrainObject.has_value() ||
            services == nullptr)
        {
            const auto existing =
                runtimes_.find(
                    target.id);

            if (existing !=
                runtimes_.end())
            {
                observerHistory_.
                    insert_or_assign(
                        target.id,
                        ObserverHistory{
                            .semanticBody =
                                existing->second->
                                    semanticBody,
                            .planet =
                                existing->second->
                                    planet.id,
                            .observer =
                                existing->second->
                                    observer
                        });

                runtimes_.erase(
                    existing);

                changed = true;
            }

            continue;
        }

        const u64 terrainRevision =
            capability->terrain->
                Revision();

        const u64 surfaceRevision =
            world_->Surfaces().
                SourceRevision();

        auto found =
            runtimes_.find(
                target.id);

        const bool needsRebind =
            found == runtimes_.end() ||
            found->second->body !=
                body ||
            found->second->semanticBody !=
                target.target->
                    semanticObject ||
            found->second->terrainObject !=
                *terrainObject ||
            found->second->
                    terrainSourceRevision !=
                terrainRevision ||
            found->second->
                    surfaceSourceRevision !=
                surfaceRevision ||
            found->second->
                    worldGeneration !=
                worldGeneration ||
            found->second->
                    universeGeneration !=
                universeGeneration;

        if (!needsRebind)
        {
            found->second->cacheStats =
                services->Cache().
                    Stats();
            continue;
        }

        std::optional<
            world::WorldPosition>
            previousObserver;

        if (found != runtimes_.end() &&
            found->second->semanticBody ==
                target.target->
                    semanticObject &&
            found->second->planet.id ==
                planet->id)
        {
            previousObserver =
                found->second->observer;
        }
        else
        {
            const auto old =
                observerHistory_.find(
                    target.id);

            if (old !=
                    observerHistory_.end() &&
                old->second.semanticBody ==
                    target.target->
                        semanticObject &&
                old->second.planet ==
                    planet->id)
            {
                previousObserver =
                    old->second.observer;
            }
        }

        auto runtime =
            BuildRuntime(
                target.id,
                target,
                previousObserver);

        if (runtime != nullptr)
        {
            observerHistory_.
                insert_or_assign(
                    target.id,
                    ObserverHistory{
                        .semanticBody =
                            runtime->semanticBody,
                        .planet =
                            runtime->planet.id,
                        .observer =
                            runtime->observer
                    });

            runtimes_.insert_or_assign(
                target.id,
                std::move(runtime));
        }
        else
        {
            runtimes_.erase(
                target.id);
        }

        changed = true;
    }

    for (auto iterator =
             runtimes_.begin();
         iterator !=
             runtimes_.end();)
    {
        if (!seen.contains(
                iterator->first))
        {
            observerHistory_.
                insert_or_assign(
                    iterator->first,
                    ObserverHistory{
                        .semanticBody =
                            iterator->second->
                                semanticBody,
                        .planet =
                            iterator->second->
                                planet.id,
                        .observer =
                            iterator->second->
                                observer
                    });

            iterator =
                runtimes_.erase(
                    iterator);
            changed = true;
        }
        else
        {
            ++iterator;
        }
    }

    return changed;
}

void StudioTerrainRuntimeBridge::Clear() noexcept
{
    runtimes_.clear();
    observerHistory_.clear();
}

bool StudioTerrainRuntimeBridge::SetObserver(
    const std::string_view viewportId,
    const world::WorldPosition& observer)
{
    const auto found =
        runtimes_.find(
            std::string(viewportId));

    if (found == runtimes_.end())
    {
        return false;
    }

    if (!ObserverValid(
            found->second->planet,
            observer))
    {
        throw std::invalid_argument(
            "Studio terrain observer must remain above the active planet surface.");
    }

    if (found->second->observer.meters ==
        observer.meters)
    {
        return false;
    }

    UpdateObserverPlan(
        *found->second,
        observer);

    observerHistory_.
        insert_or_assign(
            found->first,
            ObserverHistory{
                .semanticBody =
                    found->second->
                        semanticBody,
                .planet =
                    found->second->
                        planet.id,
                .observer =
                    found->second->
                        observer
            });

    ++found->second->
        runtimeGeneration;

    return true;
}

std::optional<
    StudioTerrainViewportRuntimeSnapshot>
StudioTerrainRuntimeBridge::Capture(
    const std::string_view viewportId) const
{
    const auto found =
        runtimes_.find(
            std::string(viewportId));

    if (found == runtimes_.end())
    {
        return std::nullopt;
    }

    return Snapshot(
        *found->second);
}

std::vector<
    StudioTerrainViewportRuntimeSnapshot>
StudioTerrainRuntimeBridge::Catalog() const
{
    std::vector<
        StudioTerrainViewportRuntimeSnapshot>
        result;

    result.reserve(
        runtimes_.size());

    for (const auto& [id, runtime] :
         runtimes_)
    {
        static_cast<void>(id);

        result.push_back(
            Snapshot(
                *runtime));
    }

    return result;
}

bool StudioTerrainRuntimeBridge::IsCurrent(
    const StudioTerrainViewportRuntimeSnapshot& snapshot) const noexcept
{
    if (world_ == nullptr ||
        !world_->HasWorld() ||
        snapshot.worldGeneration !=
            world_->Generation() ||
        snapshot.universeGeneration !=
            world_->UniverseGeneration())
    {
        return false;
    }

    const auto found =
        runtimes_.find(
            snapshot.viewportId);

    if (found == runtimes_.end())
    {
        return false;
    }

    const RuntimeState& runtime =
        *found->second;

    return
        runtime.runtimeGeneration ==
            snapshot.runtimeGeneration &&
        runtime.semanticBody ==
            snapshot.semanticBody &&
        runtime.body ==
            snapshot.body &&
        runtime.terrainObject ==
            snapshot.terrainObject &&
        runtime.terrainSourceRevision ==
            snapshot.terrainSourceRevision &&
        runtime.surfaceSourceRevision ==
            snapshot.surfaceSourceRevision;
}

const terrain::TerrainSource&
StudioTerrainRuntimeBridge::TerrainSource(
    const StudioTerrainViewportRuntimeSnapshot& snapshot) const
{
    const RuntimeState& runtime =
        RequireCurrent(snapshot);

    const auto* capability =
        world_->Surfaces().
            Registry().
            FindTerrainSurface(
                runtime.body);

    if (capability == nullptr ||
        capability->terrain == nullptr)
    {
        throw std::logic_error(
            "Current Studio terrain runtime lost its terrain capability.");
    }

    return
        *capability->terrain;
}

terrain_gpu::PersistentGpuTerrainCache&
StudioTerrainRuntimeBridge::Cache(
    const StudioTerrainViewportRuntimeSnapshot& snapshot)
{
    RuntimeState& runtime =
        RequireCurrent(snapshot);

    auto* cache =
        world_->Surfaces().
            CacheForBody(
                runtime.body);

    if (cache == nullptr)
    {
        throw std::logic_error(
            "Current Studio terrain runtime lost its M26 cache.");
    }

    runtime.cacheStats =
        cache->Stats();

    return *cache;
}

const terrain_gpu::PersistentGpuTerrainCache&
StudioTerrainRuntimeBridge::Cache(
    const StudioTerrainViewportRuntimeSnapshot& snapshot) const
{
    const RuntimeState& runtime =
        RequireCurrent(snapshot);

    const auto* cache =
        world_->Surfaces().
            CacheForBody(
                runtime.body);

    if (cache == nullptr)
    {
        throw std::logic_error(
            "Current Studio terrain runtime lost its M26 cache.");
    }

    return *cache;
}

void StudioTerrainRuntimeBridge::PublishDebugPage(
    const StudioTerrainViewportRuntimeSnapshot& snapshot,
    std::shared_ptr<
        const terrain_debug::TerrainDebugPageData>
        page)
{
    const RuntimeState& runtime =
        RequireCurrent(snapshot);

    if (page == nullptr ||
        page->Stamp().address.planet !=
            runtime.planet.id)
    {
        throw std::invalid_argument(
            "Studio terrain debug page does not belong to the current viewport planet.");
    }

    if (debugPages_ == nullptr)
    {
        throw std::logic_error(
            "Studio terrain runtime has no M29 publication registry.");
    }

    debugPages_->Publish(
        std::move(page));
}

std::unique_ptr<
    StudioTerrainRuntimeBridge::RuntimeState>
StudioTerrainRuntimeBridge::BuildRuntime(
    std::string viewportId,
    const ViewportTargetState& target,
    const std::optional<
        world::WorldPosition>
        previousObserver)
{
    if (!target.target.has_value())
    {
        return nullptr;
    }

    const auto body =
        target.target->body;

    const auto* capability =
        world_->Surfaces().
            Registry().
            FindTerrainSurface(
                body);

    const auto planet =
        world_->Surfaces().
            Registry().
            SphericalPlanetDefinition(
                body);

    const auto terrainObject =
        world_->Surfaces().
            TerrainObjectForBody(
                body);

    const auto* services =
        world_->Surfaces().
            ServicesForBody(
                body);

    if (capability == nullptr ||
        capability->terrain == nullptr ||
        !planet.has_value() ||
        !terrainObject.has_value() ||
        services == nullptr)
    {
        return nullptr;
    }

    auto runtime =
        std::make_unique<
            RuntimeState>(
                std::move(viewportId),
                world_->Generation(),
                world_->UniverseGeneration(),
                nextRuntimeGeneration_++,
                target.target->
                    semanticObject,
                body,
                *terrainObject,
                *planet,
                capability->terrain->
                    Revision(),
                world_->Surfaces().
                    SourceRevision(),
                services->Cache().
                    Stats(),
                config_.clipmap);

    world::WorldPosition observer =
        previousObserver.
            value_or(
                DefaultObserver(
                    *planet,
                    config_.
                        defaultObserverAltitudeMeters));

    if (!ObserverValid(
            *planet,
            observer))
    {
        observer =
            DefaultObserver(
                *planet,
                config_.
                    defaultObserverAltitudeMeters);
    }

    UpdateObserverPlan(
        *runtime,
        observer);

    return runtime;
}

void StudioTerrainRuntimeBridge::UpdateObserverPlan(
    RuntimeState& runtime,
    const world::WorldPosition& observer)
{
    const f64 observerRadius =
        math::Length(
            observer.meters);

    const f64 horizonArcMeters =
        world::HorizonArcDistanceMeters(
            runtime.planet.radiusMeters,
            observerRadius);

    const u32 coverageTier =
        terrain_view::
            SelectAdaptiveClipmapTierForHalfExtent(
                runtime.baseClipmap,
                config_.adaptiveCoverage,
                horizonArcMeters,
                runtime.
                    adaptiveCoverageTier);

    const terrain_view::ClipmapConfig
        activeConfig =
            terrain_view::
                ClipmapConfigForTier(
                    runtime.baseClipmap,
                    coverageTier);

    if (coverageTier !=
        runtime.adaptiveCoverageTier)
    {
        runtime.tracker =
            runtime.tracker.
                Reconfigured(
                    activeConfig);

        runtime.residency =
            terrain_stream::
                ToroidalResidency(
                    activeConfig);
    }

    runtime.adaptiveCoverageTier =
        coverageTier;

    runtime.activeClipmap =
        activeConfig;

    runtime.layout =
        terrain_view::
            BuildClipmapLayout(
                activeConfig,
                observer);

    runtime.motion =
        runtime.tracker.Update(
            observer);

    runtime.residencyUpdate =
        runtime.residency.Apply(
            runtime.motion);

    terrain_stream::
        RefreshTerrainMorphRegions(
            runtime.layout,
            runtime.motion,
            runtime.residencyUpdate);

    runtime.requests.clear();

    runtime.requests.reserve(
        runtime.layout.levels.size());

    for (u32 levelIndex = 0U;
         levelIndex <
             static_cast<u32>(
                 runtime.layout.
                     levels.size());
         ++levelIndex)
    {
        const auto& levelUpdate =
            runtime.residencyUpdate.
                levels[levelIndex];

        if (levelUpdate.
                refreshRegions.empty())
        {
            continue;
        }

        const auto& level =
            runtime.layout.levels[
                levelIndex];

        const bool hasCoarser =
            levelIndex + 1U <
            static_cast<u32>(
                runtime.layout.
                    levels.size());

        const terrain_view::ClipmapLevel*
            coarserLevel =
                hasCoarser
                    ? &runtime.layout.
                        levels[
                            levelIndex + 1U]
                    : nullptr;

        runtime.requests.push_back({
            .levelIndex =
                levelIndex,
            .resolution =
                level.gridResolution,
            .spacingMeters =
                level.
                    sampleSpacingMeters,
            .footprintMeters =
                level.
                    terrainFootprintMeters,
            .morphToCoarser =
                hasCoarser,
            .morphStartHalfExtentMeters =
                level.
                    morphStartHalfExtentMeters,
            .morphEndHalfExtentMeters =
                level.
                    morphEndHalfExtentMeters,
            .coarseSpacingMeters =
                coarserLevel != nullptr
                    ? coarserLevel->
                        sampleSpacingMeters
                    : 0.0,
            .coarseFootprintMeters =
                coarserLevel != nullptr
                    ? coarserLevel->
                        terrainFootprintMeters
                    : 0.0,
            .fineNormalFootprintMeters =
                runtime.layout.levels[
                    0U].
                    sampleSpacingMeters,
            .fineNormalEpsilonMeters =
                runtime.layout.levels[
                    0U].
                    sampleSpacingMeters,
            .centerOffsetMeters =
                runtime.motion.levels[
                    levelIndex].
                    centerOffsetMeters,
            .surfaceFrame =
                runtime.motion.levels[
                    levelIndex].
                    surfaceFrame,
            .coarseSurfaceFrame =
                hasCoarser
                    ? runtime.motion.
                        levels[
                            levelIndex + 1U].
                        surfaceFrame
                    : runtime.motion.
                        levels[
                            levelIndex].
                        surfaceFrame,
            .originX =
                levelUpdate.originX,
            .originY =
                levelUpdate.originY,
            .regions =
                levelUpdate.
                    refreshRegions
        });
    }

    runtime.observer =
        observer;

    runtime.observerPhysicalPage =
        ObserverPhysicalPage(
            runtime.planet,
            observer,
            config_.physicalPageLevel);
}

StudioTerrainViewportRuntimeSnapshot
StudioTerrainRuntimeBridge::Snapshot(
    const RuntimeState& runtime) const
{
    return {
        .viewportId =
            runtime.viewportId,
        .worldGeneration =
            runtime.worldGeneration,
        .universeGeneration =
            runtime.universeGeneration,
        .runtimeGeneration =
            runtime.runtimeGeneration,
        .semanticBody =
            runtime.semanticBody,
        .body =
            runtime.body,
        .terrainObject =
            runtime.terrainObject,
        .planet =
            runtime.planet,
        .observer =
            runtime.observer,
        .observerPhysicalPage =
            runtime.
                observerPhysicalPage,
        .physicalPageLevel =
            config_.physicalPageLevel,
        .terrainSourceRevision =
            runtime.
                terrainSourceRevision,
        .surfaceSourceRevision =
            runtime.
                surfaceSourceRevision,
        .adaptiveCoverageTier =
            runtime.
                adaptiveCoverageTier,
        .clipmap =
            runtime.activeClipmap,
        .layout =
            runtime.layout,
        .motion =
            runtime.motion,
        .residency =
            runtime.
                residencyUpdate,
        .sampleRequests =
            runtime.requests,
        .cacheStats =
            runtime.cacheStats
    };
}

StudioTerrainRuntimeBridge::RuntimeState&
StudioTerrainRuntimeBridge::RequireCurrent(
    const StudioTerrainViewportRuntimeSnapshot& snapshot)
{
    if (!IsCurrent(snapshot))
    {
        throw std::logic_error(
            "Studio terrain runtime snapshot is stale; refresh before accessing terrain services.");
    }

    return
        *runtimes_.at(
            snapshot.viewportId);
}

const StudioTerrainRuntimeBridge::RuntimeState&
StudioTerrainRuntimeBridge::RequireCurrent(
    const StudioTerrainViewportRuntimeSnapshot& snapshot) const
{
    if (!IsCurrent(snapshot))
    {
        throw std::logic_error(
            "Studio terrain runtime snapshot is stale; refresh before accessing terrain services.");
    }

    return
        *runtimes_.at(
            snapshot.viewportId);
}
} // namespace orbit::studio_session
