#include <orbit/lighting/LightingRuntimeProfiler.hpp>
#include <orbit/studio_ui/StudioRuntimeProfiler.hpp>

#include <cassert>

int main()
{
    using namespace orbit;

    lighting::ResetStudioLightingRuntimeProfiler();

    lighting::LightingSchedulerConfig config{};
    config.budget.giMs = 2.75F;
    config.emissiveGiQualityScale = 1.5F;

    lighting::LightingRequestedWork requested{
        .exactVisibilityQueries = 1200U,
        .radianceCacheUpdates = 480U,
        .reflectionQueries = 700U,
        .emissiveUpdates = 320U
    };

    lighting::LightingWorkPlan plan{
        .budget = config.budget,
        .requested = requested,
        .exactVisibilityQueries = 900U,
        .radianceCacheUpdates = 360U,
        .reflectionQueries = 350U,
        .emissiveUpdates = 240U,
        .visibilityScale = 0.75F,
        .giScale = 0.75F,
        .reflectionScale = 0.50F,
        .emissiveScale = 0.75F,
        .hardwareRayQueryAvailable = true,
        .preferHardwareRayQuery = true
    };

    lighting::PublishStudioLightingRuntimePlan(
        config,
        requested,
        plan);

    lighting::LightingGpuTimings timings{};
    const auto gi = static_cast<u32>(
        lighting::LightingGpuSection::Gi);
    timings.milliseconds[gi] = 1.85F;
    timings.valid[gi] = true;

    lighting::PublishStudioLightingRuntimeTimings(
        config,
        timings);

    const auto& lightingSnapshot =
        lighting::StudioLightingRuntimeProfiler();

    assert(lightingSnapshot.hasScheduledPlan);
    assert(lightingSnapshot.hasMeasuredTimings);
    assert(lightingSnapshot.requested.radianceCacheUpdates == 480U);
    assert(lightingSnapshot.scheduled.radianceCacheUpdates == 360U);
    assert(lightingSnapshot.scheduled.preferHardwareRayQuery);
    assert(lightingSnapshot.measured.HasSection(
        lighting::LightingGpuSection::Gi));
    assert(lightingSnapshot.measured.SectionMs(
        lighting::LightingGpuSection::Gi) == 1.85F);

    studio_ui::ResetStudioVolumeRuntimeProfiler();

    studio_ui::StudioVolumeRuntimeProfilerSnapshot volume{};
    volume.hasSelection = true;
    volume.hasFields = true;
    volume.hasSolver = true;
    volume.hasRenderer = true;
    volume.fields.resolutionX = 64U;
    volume.fields.residentTiles = 72U;
    volume.fields.totalBytes = 8U * 1024U * 1024U;
    volume.invalidatedTiles = 4U;
    volume.solver.requestedIterations = 3U;
    volume.solver.iterationsThisFrame = 2U;
    volume.solver.gpuTimingValid = true;
    volume.solver.gpuMilliseconds = 1.25F;
    volume.solver.gpuBudgetMilliseconds = 2.0F;
    volume.renderer.rendered = true;
    volume.renderer.raymarchSteps = 48U;
    volume.renderer.historyValid = true;

    studio_ui::PublishStudioVolumeRuntimeProfiler(volume);

    const auto& volumeSnapshot =
        studio_ui::StudioVolumeRuntimeProfiler();

    assert(volumeSnapshot.hasSelection);
    assert(volumeSnapshot.fields.resolutionX == 64U);
    assert(volumeSnapshot.invalidatedTiles == 4U);
    assert(volumeSnapshot.solver.iterationsThisFrame == 2U);
    assert(volumeSnapshot.solver.gpuMilliseconds == 1.25F);
    assert(volumeSnapshot.renderer.raymarchSteps == 48U);
    assert(volumeSnapshot.renderer.historyValid);

    return 0;
}
