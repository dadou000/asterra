#include <orbit/lighting/LightingScheduler.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace orbit::lighting
{
namespace
{
[[nodiscard]] f32 ClampFiniteNonNegative(
    const f32 value) noexcept
{
    return
        std::isfinite(value)
            ? std::max(value, 0.0F)
            : 0.0F;
}

[[nodiscard]] u32 ScaleCount(
    const u32 requested,
    const f32 scale) noexcept
{
    if (requested == 0U)
    {
        return 0U;
    }

    const f64 scaled =
        std::floor(
            static_cast<f64>(requested) *
            static_cast<f64>(
                std::clamp(
                    scale,
                    0.0F,
                    1.0F)));

    if (scaled <= 0.0)
    {
        return 1U;
    }

    return
        static_cast<u32>(
            std::min<f64>(
                scaled,
                requested));
}

[[nodiscard]] f32 MoveToward(
    const f32 current,
    const f32 target,
    const f32 response) noexcept
{
    return
        current +
        (target - current) *
        std::clamp(
            response,
            0.0F,
            1.0F);
}
} // namespace

f32 LightingBudget::TotalMs() const noexcept
{
    return
        ClampFiniteNonNegative(
            directLightingMs) +
        ClampFiniteNonNegative(
            visibilityMs) +
        ClampFiniteNonNegative(
            giMs) +
        ClampFiniteNonNegative(
            reflectionMs) +
        ClampFiniteNonNegative(
            emissiveMs) +
        ClampFiniteNonNegative(
            postProcessMs);
}

f32 LightingBudget::SectionMs(
    const LightingGpuSection section) const noexcept
{
    switch (section)
    {
    case LightingGpuSection::Direct:
        return ClampFiniteNonNegative(
            directLightingMs);
    case LightingGpuSection::Visibility:
        return ClampFiniteNonNegative(
            visibilityMs);
    case LightingGpuSection::Gi:
        return ClampFiniteNonNegative(
            giMs);
    case LightingGpuSection::Reflections:
        return ClampFiniteNonNegative(
            reflectionMs);
    case LightingGpuSection::Emissive:
        return ClampFiniteNonNegative(
            emissiveMs);
    case LightingGpuSection::PostProcess:
        return ClampFiniteNonNegative(
            postProcessMs);
    case LightingGpuSection::Count:
        return 0.0F;
    }

    return 0.0F;
}

f32 LightingGpuTimings::SectionMs(
    const LightingGpuSection section) const noexcept
{
    const u32 index =
        static_cast<u32>(section);

    return
        index < milliseconds.size()
            ? ClampFiniteNonNegative(
                  milliseconds[index])
            : 0.0F;
}

f32 LightingGpuTimings::TotalMs() const noexcept
{
    f32 total = 0.0F;

    for (const f32 value :
         milliseconds)
    {
        total +=
            ClampFiniteNonNegative(
                value);
    }

    return total;
}

LightingScheduler::LightingScheduler(
    LightingSchedulerConfig config)
{
    SetConfig(config);
}

void LightingScheduler::SetConfig(
    LightingSchedulerConfig config) noexcept
{
    config.minimumVisibilityScale =
        std::clamp(
            config.minimumVisibilityScale,
            0.0F,
            1.0F);
    config.minimumGiScale =
        std::clamp(
            config.minimumGiScale,
            0.0F,
            1.0F);
    config.minimumReflectionScale =
        std::clamp(
            config.minimumReflectionScale,
            0.0F,
            1.0F);
    config.minimumEmissiveScale =
        std::clamp(
            config.minimumEmissiveScale,
            0.0F,
            1.0F);
    config.overloadResponse =
        std::clamp(
            config.overloadResponse,
            0.0F,
            1.0F);
    config.recoveryResponse =
        std::clamp(
            config.recoveryResponse,
            0.0F,
            1.0F);
    config.hardwareRayQueryPreferenceThreshold =
        std::clamp(
            config.hardwareRayQueryPreferenceThreshold,
            0.0F,
            1.0F);

    config_ = config;
}

const LightingSchedulerConfig&
LightingScheduler::Config() const noexcept
{
    return config_;
}

f32 LightingScheduler::ScaleFor(
    const LightingGpuSection section,
    const f32 minimumScale,
    const f32 previousScale) const noexcept
{
    if (!hasTimings_)
    {
        return 1.0F;
    }

    const f32 budget =
        config_.budget.SectionMs(section);
    const f32 measured =
        smoothed_.SectionMs(section);

    if (budget <= 0.0F)
    {
        return 0.0F;
    }

    if (measured <= 1.0e-5F)
    {
        return
            MoveToward(
                previousScale,
                1.0F,
                config_.recoveryResponse);
    }

    const f32 target =
        std::clamp(
            budget / measured,
            minimumScale,
            1.0F);

    const bool overloaded =
        measured > budget;

    return
        MoveToward(
            previousScale,
            target,
            overloaded
                ? config_.overloadResponse
                : config_.recoveryResponse);
}

void LightingScheduler::RecordGpuTimings(
    const LightingGpuTimings& timings) noexcept
{
    constexpr f32 kTimingEwmaAlpha =
        0.20F;

    if (!hasTimings_)
    {
        smoothed_ = timings;
        hasTimings_ = true;
    }
    else
    {
        for (u32 index = 0U;
             index < kLightingGpuSectionCount;
             ++index)
        {
            const f32 sample =
                ClampFiniteNonNegative(
                    timings.milliseconds[index]);

            smoothed_.milliseconds[index] =
                MoveToward(
                    smoothed_.milliseconds[index],
                    sample,
                    kTimingEwmaAlpha);
        }
    }

    visibilityScale_ =
        ScaleFor(
            LightingGpuSection::Visibility,
            config_.minimumVisibilityScale,
            visibilityScale_);

    giScale_ =
        ScaleFor(
            LightingGpuSection::Gi,
            config_.minimumGiScale,
            giScale_);

    reflectionScale_ =
        ScaleFor(
            LightingGpuSection::Reflections,
            config_.minimumReflectionScale,
            reflectionScale_);

    emissiveScale_ =
        ScaleFor(
            LightingGpuSection::Emissive,
            config_.minimumEmissiveScale,
            emissiveScale_);
}

LightingWorkPlan LightingScheduler::BuildPlan(
    const LightingRequestedWork& requested,
    const bool hardwareRayQueryAvailable) const noexcept
{
    LightingWorkPlan plan{
        .budget = config_.budget,
        .requested = requested,
        .exactVisibilityQueries =
            ScaleCount(
                requested.exactVisibilityQueries,
                visibilityScale_),
        .radianceCacheUpdates =
            ScaleCount(
                requested.radianceCacheUpdates,
                giScale_),
        .reflectionQueries =
            ScaleCount(
                requested.reflectionQueries,
                reflectionScale_),
        .emissiveUpdates =
            ScaleCount(
                requested.emissiveUpdates,
                emissiveScale_),
        .visibilityScale =
            visibilityScale_,
        .giScale =
            giScale_,
        .reflectionScale =
            reflectionScale_,
        .emissiveScale =
            emissiveScale_,
        .hardwareRayQueryAvailable =
            hardwareRayQueryAvailable
    };

    // Capability changes the preferred implementation of the existing
    // visibility work, never the quantity of requested work.
    plan.preferHardwareRayQuery =
        hardwareRayQueryAvailable &&
        plan.visibilityScale >=
            config_.
                hardwareRayQueryPreferenceThreshold;

    return plan;
}

const LightingGpuTimings&
LightingScheduler::SmoothedTimings() const noexcept
{
    return smoothed_;
}

LightingTimestampRecorder::LightingTimestampRecorder(
    rhi::Device& device,
    const u32 framesInFlight)
    : device_(&device),
      timestampPeriodNanoseconds_(
          device.TimestampPeriodNanoseconds())
{
    if (framesInFlight == 0U)
    {
        throw std::invalid_argument(
            "Lighting timestamp recorder requires at least one frame slot.");
    }

    pools_.reserve(framesInFlight);

    for (u32 index = 0U;
         index < framesInFlight;
         ++index)
    {
        pools_.push_back(
            device.CreateTimestampQueryPool(
                kLightingGpuSectionCount *
                2U));
    }
}

u32 LightingTimestampRecorder::QueryIndex(
    const LightingGpuSection section,
    const bool end) noexcept
{
    return
        static_cast<u32>(section) *
            2U +
        (end ? 1U : 0U);
}

void LightingTimestampRecorder::BeginFrame(
    rhi::CommandList& commands,
    const u32 frameSlot)
{
    if (frameSlot >= pools_.size())
    {
        throw std::out_of_range(
            "Lighting timestamp frame slot is out of range.");
    }

    commands.ResetTimestampQueryPool(
        *pools_[frameSlot],
        0U,
        kLightingGpuSectionCount *
            2U);
}

void LightingTimestampRecorder::BeginSection(
    rhi::CommandList& commands,
    const u32 frameSlot,
    const LightingGpuSection section)
{
    if (frameSlot >= pools_.size() ||
        section == LightingGpuSection::Count)
    {
        throw std::out_of_range(
            "Lighting timestamp section/frame slot is invalid.");
    }

    commands.WriteTimestamp(
        *pools_[frameSlot],
        QueryIndex(
            section,
            false));
}

void LightingTimestampRecorder::EndSection(
    rhi::CommandList& commands,
    const u32 frameSlot,
    const LightingGpuSection section)
{
    if (frameSlot >= pools_.size() ||
        section == LightingGpuSection::Count)
    {
        throw std::out_of_range(
            "Lighting timestamp section/frame slot is invalid.");
    }

    commands.WriteTimestamp(
        *pools_[frameSlot],
        QueryIndex(
            section,
            true));
}

std::optional<LightingGpuTimings>
LightingTimestampRecorder::ResolveCompletedFrame(
    const u32 frameSlot) const
{
    if (frameSlot >= pools_.size())
    {
        throw std::out_of_range(
            "Lighting timestamp frame slot is out of range.");
    }

    std::array<
        u64,
        kLightingGpuSectionCount * 2U>
        ticks{};

    if (!pools_[frameSlot]->TryGetResults(
            0U,
            static_cast<u32>(
                ticks.size()),
            ticks.data()))
    {
        return std::nullopt;
    }

    LightingGpuTimings result;

    for (u32 section = 0U;
         section < kLightingGpuSectionCount;
         ++section)
    {
        const u64 begin =
            ticks[section * 2U];
        const u64 end =
            ticks[section * 2U + 1U];

        if (end < begin)
        {
            result.milliseconds[section] =
                0.0F;
            continue;
        }

        const f64 nanoseconds =
            static_cast<f64>(
                end - begin) *
            timestampPeriodNanoseconds_;

        result.milliseconds[section] =
            static_cast<f32>(
                nanoseconds /
                1'000'000.0);
    }

    return result;
}

u32 LightingTimestampRecorder::FramesInFlight() const noexcept
{
    return
        static_cast<u32>(
            pools_.size());
}
} // namespace orbit::lighting
