#include <orbit/studio_session/StudioCelestialValidationScenario.hpp>

#include <filesystem>

int main()
{
    using namespace orbit;

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-v006-celestial-scenario-" +
         documents::ProjectId::Random().
             ToString());

    std::filesystem::remove_all(root);

    const auto report =
        studio_session::
            RunStudioCelestialValidationScenario(
                root);

    if (!report.success)
        return 1;

    if (!report.helion ||
        !report.asterra ||
        !report.luma ||
        !report.umbra)
    {
        return 2;
    }

    if (!report.
            terrainGroundRuntimeAvailable ||
        !report.
            terrainOrbitRuntimeAvailable ||
        !report.atmosphereResolved ||
        !report.oceanResolved ||
        !report.cloudsResolved)
    {
        return 3;
    }

    if (!report.eclipseDetected ||
        !report.
            simulationTimeChangesOrbit ||
        !report.
            fullRepresentationLadderObserved)
    {
        return 4;
    }

    if (!report.binaryStarStressPassed ||
        !report.ringedGiantStressPassed ||
        !report.airlessBodyStressPassed)
    {
        return 5;
    }

    if (!report.roundTrip.success ||
        !report.
            realDeviceVisualSmokeRequired)
    {
        return 6;
    }

    std::filesystem::remove_all(root);
    return 0;
}
