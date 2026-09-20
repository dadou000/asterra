#pragma once

#include <orbit/scene/ObjectStore.hpp>
#include <orbit/terrain/TerrainContracts.hpp>
#include <orbit/terrain_biome/BiomeService.hpp>
#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_gpu/PersistentGpuTerrainCache.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace orbit::studio_session
{
class StudioSession;
class StudioTerrainRebuildScheduler;

struct StudioTerrainProcessStatus
{
    u32 streamPowerIterations{0U};
    u32 hydraulicIterations{0U};
    u32 thermalMaximumIterations{0U};
    u32 aeolianIterations{0U};
    u32 glacialIterations{0U};

    bool riverMeanders{false};
    u32 riverMeanderIterations{0U};
    bool riverCutoffs{false};

    bool coastalEnabled{false};
    u32 coastalHydrodynamicSteps{0U};
};

struct StudioTerrainServiceStatusSnapshot
{
    scene::ObjectId terrainObject{};
    universe::BodyId body{};

    // Current authority/service revisions. These are copied values so callers
    // never retain TerrainBodyServices references across composition rebuilds.
    u64 semanticRevision{0U};
    u64 surfaceSourceRevision{0U};
    u64 geologyRevision{0U};
    u64 biomeRevision{0U};
    std::optional<u64> terrainSourceRevision;
    std::optional<u64> physicalRevisionFingerprint;

    terrain_geology::RockTypeId defaultBedrock{};
    std::string defaultBedrockName;

    bool exposedSurfaceAvailable{false};
    std::string exposedSurfaceName;

    StudioTerrainProcessStatus processes{};

    terrain_biome::BiomeId baseBiome{};
    std::string baseBiomeName;
    u32 optionalBiomeCount{0U};

    terrain_gpu::PersistentGpuTerrainCacheStats cacheStats{};

    std::optional<terrain::PhysicalTerrainPageAddress>
        selectedPhysicalPage;
    std::optional<u8> selectedPhysicalLod;
    std::string selectedViewport;

    bool rebuildSchedulerAttached{false};
    bool regenerationPaused{false};
    u32 rebuildPages{0U};
    u32 dirtyPages{0U};
    u32 queuedPages{0U};
    u32 buildingPages{0U};
    u32 uploadingPages{0U};
    u32 failedPages{0U};
    std::string selectedRebuildState;
    std::string lastRegenerationReason;
};

// M07 diagnostic reader. It copies from the current StudioSession service graph
// and optional M06 scheduler; it never mutates semantic or physical authority.
class StudioTerrainStatusInspector
{
public:
    [[nodiscard]] static std::optional<
        StudioTerrainServiceStatusSnapshot>
    Capture(
        StudioSession& session,
        scene::ObjectId terrainObject,
        const StudioTerrainRebuildScheduler* scheduler = nullptr,
        std::string_view preferredViewport = {});
};
} // namespace orbit::studio_session
