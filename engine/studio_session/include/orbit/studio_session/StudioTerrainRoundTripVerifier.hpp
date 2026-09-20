#pragma once

#include <orbit/scene/ObjectStore.hpp>
#include <orbit/studio_session/StudioWorkspace.hpp>
#include <orbit/terrain/TerrainContracts.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace orbit::studio_session
{
struct StudioTerrainRoundTripReport
{
    bool success{false};

    std::filesystem::path projectManifest;
    std::filesystem::path worldPath;

    scene::ObjectId semanticBody{};
    scene::ObjectId terrainObject{};
    terrain::PhysicalTerrainPageAddress comparisonPage{};

    u64 semanticFingerprintBefore{0U};
    u64 semanticFingerprintAfter{0U};

    u64 physicalFingerprintBefore{0U};
    u64 physicalFingerprintAfter{0U};

    bool semanticIdsPreserved{false};
    bool derivedCacheFreshAfterReopen{false};
    bool debugResidencyFreshAfterReopen{false};
    bool comparisonPagePreserved{false};

    std::string failureStage;
    std::string diagnostic;
};

// M14 developer validation action. This deliberately performs a real
// StudioWorkspace close/open cycle: authored state is checkpointed, the old
// StudioSession is destroyed, a fresh one is constructed, derived terrain
// residency is verified empty, and the same physical page is regenerated.
//
// The operation mutates only normal workspace lifecycle/presentation interest;
// it never edits semantic terrain authority.
[[nodiscard]] StudioTerrainRoundTripReport
VerifyStudioTerrainRoundTrip(
    StudioWorkspace& workspace,
    std::string_view viewportId = "studio.primary");
} // namespace orbit::studio_session
