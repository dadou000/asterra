#include <orbit/lighting/LightingScheduler.hpp>

#include <cmath>

int main()
{
    using namespace orbit::lighting;

    LightingScheduler scheduler({
        .budget = {
            .directLightingMs = 0.8F,
            .visibilityMs = 1.0F,
            .giMs = 2.0F,
            .reflectionMs = 1.0F,
            .emissiveMs = 0.5F,
            .postProcessMs = 0.7F
        },
        .minimumVisibilityScale = 0.10F,
        .minimumGiScale = 0.10F,
        .minimumReflectionScale = 0.05F,
        .minimumEmissiveScale = 0.10F,
        .overloadResponse = 1.0F,
        .recoveryResponse = 0.25F,
        .hardwareRayQueryPreferenceThreshold = 0.35F
    });

    const LightingRequestedWork requested{
        .exactVisibilityQueries = 1000U,
        .radianceCacheUpdates = 400U,
        .reflectionQueries = 600U,
        .emissiveUpdates = 200U
    };

    const auto noRtInitial =
        scheduler.BuildPlan(
            requested,
            false);

    const auto rtInitial =
        scheduler.BuildPlan(
            requested,
            true);

    // Capability must never grow the work or budget.
    if (noRtInitial.TotalBudgetMs() !=
            rtInitial.TotalBudgetMs() ||
        noRtInitial.exactVisibilityQueries !=
            rtInitial.exactVisibilityQueries ||
        noRtInitial.radianceCacheUpdates !=
            rtInitial.radianceCacheUpdates ||
        noRtInitial.reflectionQueries !=
            rtInitial.reflectionQueries ||
        noRtInitial.emissiveUpdates !=
            rtInitial.emissiveUpdates)
    {
        return 1;
    }

    LightingGpuTimings overloaded;
    overloaded.milliseconds[
        static_cast<u32>(
            LightingGpuSection::Visibility)] =
        4.0F;
    overloaded.milliseconds[
        static_cast<u32>(
            LightingGpuSection::Gi)] =
        8.0F;
    overloaded.milliseconds[
        static_cast<u32>(
            LightingGpuSection::Reflections)] =
        4.0F;
    overloaded.milliseconds[
        static_cast<u32>(
            LightingGpuSection::Emissive)] =
        2.0F;

    scheduler.RecordGpuTimings(
        overloaded);

    const auto pressured =
        scheduler.BuildPlan(
            requested,
            true);

    if (pressured.exactVisibilityQueries >=
            requested.exactVisibilityQueries ||
        pressured.radianceCacheUpdates >=
            requested.radianceCacheUpdates ||
        pressured.reflectionQueries >=
            requested.reflectionQueries ||
        pressured.emissiveUpdates >=
            requested.emissiveUpdates)
    {
        return 2;
    }

    // 1/4 measured-to-budget ratio with response=1 should target roughly
    // 25% work, independently for these four scalable features.
    if (pressured.exactVisibilityQueries != 250U ||
        pressured.radianceCacheUpdates != 100U ||
        pressured.reflectionQueries != 150U ||
        pressured.emissiveUpdates != 50U)
    {
        return 3;
    }

    const f32 totalBudget =
        scheduler.Config().budget.TotalMs();

    if (std::abs(
            totalBudget -
            6.0F) >
        1.0e-5F)
    {
        return 4;
    }

    // Recovery is deliberately gradual.
    LightingGpuTimings cool;
    scheduler.RecordGpuTimings(cool);

    const auto recovering =
        scheduler.BuildPlan(
            requested,
            false);

    if (recovering.exactVisibilityQueries <=
            pressured.exactVisibilityQueries ||
        recovering.exactVisibilityQueries >=
            requested.exactVisibilityQueries)
    {
        return 5;
    }

    return 0;
}
