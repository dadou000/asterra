#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/studio_session/StudioTerrainValidationScenario.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr
            << "Studio terrain validation scenario test failed.\n";
        std::exit(1);
    }
}
} // namespace

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-studio-m15-validation-" +
         orbit::documents::ProjectId::Random().
             ToString());

    std::filesystem::remove_all(root);

    const auto report =
        orbit::studio_session::
            RunStudioTerrainValidationScenario(
                root,
                "studio.primary");

    if (!report.success)
    {
        std::cerr
            << "M15 failure stage: "
            << report.failureStage
            << " | "
            << report.diagnostic
            << '\n';

        for (const auto& step :
             report.steps)
        {
            std::cerr
                << (step.passed ? "[PASS] " : "[FAIL] ")
                << step.name;

            if (!step.diagnostic.empty())
            {
                std::cerr
                    << " | "
                    << step.diagnostic;
            }

            std::cerr << '\n';
        }
    }

    Check(report.success);
    Check(report.semanticBody.IsValid());
    Check(report.terrainObject.IsValid());
    Check(report.canyonConstraint.IsValid());
    Check(report.biomeObject.IsValid());
    Check(report.biomeMask.IsValid());

    Check(
        report.semanticRevisionAfterEdits !=
        0U);
    Check(
        report.terrainSourceRevisionAfterEdits !=
        0U);
    Check(
        report.queuedInvalidations !=
        0U);

    Check(
        report.debugFieldsAvailable !=
        0U);
    Check(
        report.debugPhysicalLodAvailable);
    Check(
        report.debugBiomeWeightsAvailable);

    Check(
        report.cacheStats.residentPages !=
        0U);
    Check(
        report.cacheStats.residentBytes !=
        0U);

    Check(
        report.performance.
            hasTerrainRuntime);
    Check(
        report.performance.
            residentTrackedPages <=
        5U);
    Check(
        report.performance.
            peakOutstandingPages <=
        5U);
    Check(
        !report.performance.
             buildConfiguration.empty());
    Check(
        !report.performance.
             sourceCommit.empty());

    Check(report.roundTrip.success);
    Check(
        report.roundTrip.semanticBody ==
        report.semanticBody);
    Check(
        report.roundTrip.terrainObject ==
        report.terrainObject);
    Check(
        report.roundTrip.semanticIdsPreserved);
    Check(
        report.roundTrip.
            terrainSourceRevisionPreserved);
    Check(
        report.roundTrip.
            derivedCacheFreshAfterReopen);
    Check(
        report.roundTrip.
            debugResidencyFreshAfterReopen);
    Check(
        report.roundTrip.
            comparisonPagePreserved);
    Check(
        report.roundTrip.
            semanticFingerprintBefore ==
        report.roundTrip.
            semanticFingerprintAfter);
    Check(
        report.roundTrip.
            physicalFingerprintBefore ==
        report.roundTrip.
            physicalFingerprintAfter);

    for (const auto& step :
         report.steps)
    {
        Check(step.passed);
    }

    std::filesystem::remove_all(root);
    return 0;
}
