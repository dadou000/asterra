#pragma once

#include <orbit/scene/ObjectStore.hpp>
#include <orbit/studio_session/StudioTerrainRoundTripVerifier.hpp>
#include <orbit/terrain/TerrainContracts.hpp>
#include <orbit/terrain_gpu/PersistentGpuTerrainCache.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::studio_session
{
struct StudioTerrainValidationScenarioStep
{
    std::string name;
    bool passed{false};
    std::string diagnostic;
};

struct StudioTerrainValidationScenarioReport
{
    bool success{false};

    std::filesystem::path projectRoot;
    std::filesystem::path projectManifest;
    std::filesystem::path worldPath;

    scene::ObjectId semanticBody{};
    scene::ObjectId terrainObject{};
    scene::ObjectId canyonConstraint{};
    scene::ObjectId biomeObject{};
    scene::ObjectId biomeMask{};

    terrain::PhysicalTerrainPageAddress comparisonPage{};

    u64 semanticRevisionAfterEdits{0U};
    u64 terrainSourceRevisionAfterEdits{0U};
    u32 queuedInvalidations{0U};

    u32 debugFieldsAvailable{0U};
    bool debugPhysicalLodAvailable{false};
    bool debugBiomeWeightsAvailable{false};

    terrain_gpu::PersistentGpuTerrainCacheStats cacheStats{};

    StudioTerrainRoundTripReport roundTrip{};
    std::vector<StudioTerrainValidationScenarioStep> steps;

    std::string failureStage;
    std::string diagnostic;
};

// M15 deterministic editor-driven acceptance scenario. The scenario creates an
// isolated real Orbit Studio project/world, drives the same CommandRegistry,
// SurfaceAuthoringModel, M27 invalidation, M12/M29 runtime and M14 workspace
// lifecycle used by the interactive editor, and returns a structured report.
//
// rootDirectory must not already exist. The caller owns cleanup so a failed
// scenario can leave its project on disk for inspection.
[[nodiscard]] StudioTerrainValidationScenarioReport
RunStudioTerrainValidationScenario(
    const std::filesystem::path& rootDirectory,
    std::string_view viewportId = "studio.primary");
} // namespace orbit::studio_session
