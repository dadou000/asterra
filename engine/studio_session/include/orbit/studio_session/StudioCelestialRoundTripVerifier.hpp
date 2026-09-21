#pragma once

#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/studio_session/StudioWorkspace.hpp>

#include <filesystem>
#include <string>

namespace orbit::studio_session
{
struct StudioCelestialRoundTripReport
{
    bool success{false};

    std::filesystem::path projectManifest;
    std::filesystem::path worldPath;
    scene::ObjectId semanticBody{};

    u64 semanticFingerprintBefore{0U};
    u64 semanticFingerprintAfter{0U};

    u64 runtimeOrbitFingerprintBefore{0U};
    u64 runtimeOrbitFingerprintAfter{0U};

    u64 derivedAppearanceFingerprintBefore{0U};
    u64 derivedAppearanceFingerprintAfter{0U};

    u64 representationFingerprintBefore{0U};
    u64 representationFingerprintAfter{0U};

    bool semanticIdsPreserved{false};
    bool provenancePreserved{false};
    bool freshWorkspaceRecomposition{false};
    bool derivedProductsRegenerated{false};

    std::string failureStage;
    std::string diagnostic;
};

[[nodiscard]] StudioCelestialRoundTripReport
VerifyStudioCelestialRoundTrip(
    documents::ProjectDocument& project,
    StudioSession& session,
    scene::ObjectId semanticBody);

[[nodiscard]] StudioCelestialRoundTripReport
VerifyStudioCelestialRoundTrip(
    StudioWorkspace& workspace,
    scene::ObjectId semanticBody);
} // namespace orbit::studio_session
