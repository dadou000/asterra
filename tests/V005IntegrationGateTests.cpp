#include <orbit/studio_session/StudioTerrainValidationScenario.hpp>
#include <orbit/studio_ui/StudioViewportRenderer.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
constexpr orbit::u32 kMaximumTransitionOutstandingPages = 10U;

void Require(
    const bool condition,
    const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(
            message);
    }
}
} // namespace

int main()
{
    using namespace orbit;

    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-v005-m18-gate-" +
         documents::ProjectId::Random().
             ToString());

    std::filesystem::remove_all(root);

    try
    {
        const auto report =
            studio_session::
                RunStudioTerrainValidationScenario(
                    root,
                    "studio.primary");

        Require(
            report.success,
            "M18: the real Studio terrain acceptance scenario failed.");

        Require(
            studio_ui::
                SelectStudioViewportPresentation(
                    studio_session::
                        ViewportMode::
                            Perspective,
                    true,
                    true,
                    false,
                    false) ==
                studio_ui::
                    StudioViewportPresentation::
                        ProductionTerrain,
            "M18: a terrain-bearing Perspective viewport must select the production terrain renderer.");

        Require(
            report.semanticBody.IsValid() &&
            report.terrainObject.IsValid(),
            "M18: clean project did not produce a valid rocky planet and Terrain Surface.");

        Require(
            report.canyonConstraint.IsValid(),
            "M18: viewport-style canyon authority was not committed.");

        Require(
            report.biomeObject.IsValid() &&
            report.biomeMask.IsValid(),
            "M18: biome authoring/paint authority was not committed.");

        Require(
            report.queuedInvalidations > 0U,
            "M18: authoring did not flow through M27 invalidation.");

        Require(
            report.debugFieldsAvailable > 0U &&
            report.debugPhysicalLodAvailable &&
            report.debugBiomeWeightsAvailable,
            "M18: live M29 inspection did not expose required production fields.");

        Require(
            report.performance.hasTerrainRuntime,
            "M18: M16 diagnostics did not observe the production terrain runtime.");

        if (report.performance.residentTrackedPages >
                kMaximumTransitionOutstandingPages ||
            report.performance.peakOutstandingPages >
                kMaximumTransitionOutstandingPages)
        {
            std::cerr
                << "M18 bounded-page diagnostics: resident="
                << report.performance.residentTrackedPages
                << ", peak-outstanding="
                << report.performance.peakOutstandingPages
                << '\n';
        }

        Require(
            report.performance.residentTrackedPages <=
                kMaximumTransitionOutstandingPages &&
            report.performance.peakOutstandingPages <=
                kMaximumTransitionOutstandingPages,
            "M18: terrain page residency/queue exceeded two bounded observer neighborhoods.");

        Require(
            !report.performance.
                 buildConfiguration.empty() &&
            !report.performance.
                 sourceCommit.empty(),
            "M18: diagnostics report does not identify build configuration/source commit.");

        Require(
            report.cacheStats.residentPages ==
                0U &&
            report.cacheStats.residentBytes ==
                0U,
            "M18: the headless gate must not fabricate renderer-owned M26 GPU residency.");

        Require(
            report.roundTrip.success &&
            report.roundTrip.semanticIdsPreserved &&
            report.roundTrip.
                terrainSourceRevisionPreserved &&
            report.roundTrip.
                derivedCacheFreshAfterReopen &&
            report.roundTrip.
                debugResidencyFreshAfterReopen &&
            report.roundTrip.
                comparisonPagePreserved,
            "M18: save/reopen did not preserve semantic authority while rebuilding derived state.");

        Require(
            report.roundTrip.
                semanticFingerprintBefore ==
            report.roundTrip.
                semanticFingerprintAfter,
            "M18: authored semantic fingerprint changed across save/reopen.");

        Require(
            report.roundTrip.
                physicalFingerprintBefore ==
            report.roundTrip.
                physicalFingerprintAfter,
            "M18: regenerated physical/M29 result changed across save/reopen.");

        std::filesystem::remove_all(root);

        std::cout
            << "Orbit V0.0.5 M18 final Studio terrain integration gate passed.\n";

        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::filesystem::remove_all(root);

        std::cerr
            << error.what()
            << '\n';

        return EXIT_FAILURE;
    }
}
