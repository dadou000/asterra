#pragma once

#include <orbit/lighting/LightingScheduler.hpp>

namespace orbit::lighting
{
struct LightingRuntimeProfilerSnapshot
{
    LightingSchedulerConfig config{};
    LightingGpuTimings measured{};
    LightingRequestedWork requested{};
    LightingWorkPlan scheduled{};

    bool hasMeasuredTimings{false};
    bool hasScheduledPlan{false};
};

[[nodiscard]] inline LightingRuntimeProfilerSnapshot&
MutableStudioLightingRuntimeProfiler() noexcept
{
    static LightingRuntimeProfilerSnapshot snapshot{};
    return snapshot;
}

[[nodiscard]] inline const LightingRuntimeProfilerSnapshot&
StudioLightingRuntimeProfiler() noexcept
{
    return MutableStudioLightingRuntimeProfiler();
}

inline void PublishStudioLightingRuntimePlan(
    const LightingSchedulerConfig& config,
    const LightingRequestedWork& requested,
    const LightingWorkPlan& scheduled) noexcept
{
    auto& snapshot =
        MutableStudioLightingRuntimeProfiler();

    snapshot.config = config;
    snapshot.requested = requested;
    snapshot.scheduled = scheduled;
    snapshot.hasScheduledPlan = true;
}

inline void PublishStudioLightingRuntimeTimings(
    const LightingSchedulerConfig& config,
    const LightingGpuTimings& measured) noexcept
{
    auto& snapshot =
        MutableStudioLightingRuntimeProfiler();

    snapshot.config = config;
    snapshot.measured = measured;
    snapshot.hasMeasuredTimings = true;
}

inline void ResetStudioLightingRuntimeProfiler() noexcept
{
    MutableStudioLightingRuntimeProfiler() = {};
}
} // namespace orbit::lighting
