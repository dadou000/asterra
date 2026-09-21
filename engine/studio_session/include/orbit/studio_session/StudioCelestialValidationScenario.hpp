#pragma once

#include <orbit/scene/ObjectStore.hpp>
#include <orbit/studio_session/StudioCelestialRoundTripVerifier.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace orbit::studio_session
{
struct StudioCelestialValidationScenarioStep
{
    std::string name;
    bool passed{false};
    std::string diagnostic;
};

struct StudioCelestialValidationScenarioReport
{
    bool success{false};

    std::filesystem::path projectRoot;
    std::filesystem::path projectManifest;
    std::filesystem::path worldPath;

    scene::ObjectId helion{};
    scene::ObjectId asterra{};
    scene::ObjectId luma{};
    scene::ObjectId umbra{};
    scene::ObjectId companionStar{};
    scene::ObjectId ringedGiant{};
    scene::ObjectId airlessBody{};

    bool terrainGroundRuntimeAvailable{false};
    bool terrainOrbitRuntimeAvailable{false};
    bool atmosphereResolved{false};
    bool oceanResolved{false};
    bool cloudsResolved{false};
    bool eclipseDetected{false};
    bool simulationTimeChangesOrbit{false};
    bool fullRepresentationLadderObserved{false};
    bool binaryStarStressPassed{false};
    bool ringedGiantStressPassed{false};
    bool airlessBodyStressPassed{false};
    bool realDeviceVisualSmokeRequired{true};

    StudioCelestialRoundTripReport roundTrip{};

    std::vector<StudioCelestialValidationScenarioStep>
        steps;

    std::string failureStage;
    std::string diagnostic;
};

[[nodiscard]] StudioCelestialValidationScenarioReport
RunStudioCelestialValidationScenario(
    const std::filesystem::path& rootDirectory,
    std::string_view viewportId = "studio.primary");
} // namespace orbit::studio_session
