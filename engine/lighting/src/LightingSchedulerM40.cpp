#include <orbit/lighting/LightingScheduler.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

// Preserve the validated M10 scheduler implementation and rename only the two
// methods whose policy is extended by M40.
#define RecordGpuTimings RecordGpuTimingsBase
#define BuildPlan BuildPlanBase
#include "LightingScheduler.cpp"
#undef BuildPlan
#undef RecordGpuTimings

namespace orbit::lighting
{
namespace
{
std::optional<LightingSchedulerConfig> gStudioRuntimeConfig;

[[nodiscard]] u32 ScaleQualityCount(
    const u32 requested,
    const f32 scale) noexcept
{
    if (requested == 0U || scale <= 0.0F)
    {
        return 0U;
    }

    const f64 scaled =
        std::round(
            static_cast<f64>(requested) *
            static_cast<f64>(
                std::clamp(scale, 0.0F, 4.0F)));

    return static_cast<u32>(
        std::clamp<f64>(
            scaled,
            1.0,
            static_cast<f64>(
                std::numeric_limits<u32>::max())));
}

[[nodiscard]] LightingSchedulerConfig EffectiveConfig(
    const LightingSchedulerConfig& local) noexcept
{
    auto result =
        gStudioRuntimeConfig.value_or(local);

    result.emissiveGiQualityScale =
        std::clamp(
            result.emissiveGiQualityScale,
            0.0F,
            4.0F);
    result.hardwareRayQueryPreferenceThreshold =
        std::clamp(
            result.hardwareRayQueryPreferenceThreshold,
            0.0F,
            1.0F);
    return result;
}
} // namespace

void SetStudioLightingRuntimeConfig(
    std::optional<LightingSchedulerConfig> config) noexcept
{
    if (config.has_value())
    {
        config->emissiveGiQualityScale =
            std::clamp(
                config->emissiveGiQualityScale,
                0.0F,
                4.0F);
        config->hardwareRayQueryPreferenceThreshold =
            std::clamp(
                config->hardwareRayQueryPreferenceThreshold,
                0.0F,
                1.0F);
    }

    gStudioRuntimeConfig =
        std::move(config);
}

std::optional<LightingSchedulerConfig>
StudioLightingRuntimeConfig() noexcept
{
    return gStudioRuntimeConfig;
}

void LightingScheduler::RecordGpuTimings(
    const LightingGpuTimings& timings) noexcept
{
    const auto local = config_;
    config_ = EffectiveConfig(local);
    RecordGpuTimingsBase(timings);
    config_ = local;
}

LightingWorkPlan LightingScheduler::BuildPlan(
    const LightingRequestedWork& requested,
    const bool hardwareRayQueryAvailable) const noexcept
{
    const auto effective =
        EffectiveConfig(config_);

    LightingRequestedWork qualityRequested =
        requested;
    qualityRequested.emissiveUpdates =
        ScaleQualityCount(
            requested.emissiveUpdates,
            effective.emissiveGiQualityScale);

    // Build from the existing M10 scale state so capability never changes the
    // amount of work. The M40 quality multiplier is an explicit user policy.
    LightingWorkPlan plan{
        .budget = effective.budget,
        .requested = requested,
        .exactVisibilityQueries =
            ScaleCount(
                qualityRequested.exactVisibilityQueries,
                visibilityScale_),
        .radianceCacheUpdates =
            ScaleCount(
                qualityRequested.radianceCacheUpdates,
                giScale_),
        .reflectionQueries =
            ScaleCount(
                qualityRequested.reflectionQueries,
                reflectionScale_),
        .emissiveUpdates =
            ScaleCount(
                qualityRequested.emissiveUpdates,
                emissiveScale_),
        .visibilityScale = visibilityScale_,
        .giScale = giScale_,
        .reflectionScale = reflectionScale_,
        .emissiveScale = emissiveScale_,
        .hardwareRayQueryAvailable =
            hardwareRayQueryAvailable
    };

    plan.preferHardwareRayQuery =
        hardwareRayQueryAvailable &&
        effective.hardwareRayQueryEnabled &&
        plan.visibilityScale >=
            effective.hardwareRayQueryPreferenceThreshold;

    return plan;
}
} // namespace orbit::lighting
