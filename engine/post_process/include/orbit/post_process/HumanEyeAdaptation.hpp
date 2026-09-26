#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/post_process/LuminanceHistogram.hpp>

namespace orbit::post_process
{
struct HumanEyeAdaptationConfig
{
    // Photopic state follows a robust mix of the frame median and upper
    // luminance distribution. Values are in log2 scene-luminance stops.
    f32 photopicP50Weight{0.78F};
    f32 photopicP95Weight{0.22F};
    f32 photopicBrightenSeconds{0.18F};
    f32 photopicDarkenSeconds{1.25F};

    // M25 calibrated ceiling: the adaptation state cannot chase a robust
    // bright-scene target above this scene-linear log2 luminance. Scene HDR
    // itself is never clamped; excess remains available for highlight FX.
    f32 photopicCeilingLog2{2.0F};
    f32 photopicCeilingRecoverySeconds{0.12F};

    // Exposure is presentation-only and maps the adapted luminance to middle
    // gray. Bounds protect the display path from invalid/extreme values.
    f32 exposureMiddleGray{0.18F};
    f32 minimumExposureScale{1.0F / 4096.0F};
    f32 maximumExposureScale{4096.0F};

    // Dark adaptation is deliberately separate from photopic exposure.
    // It accumulates only when the median is below this scene threshold.
    f32 darkThresholdLog2{-3.0F};
    f32 darkFullLog2{-9.0F};
    f32 darkAdaptSeconds{18.0F};
    f32 darkResetSeconds{0.30F};

    // Retinal/display overload reacts to highlights relative to the current
    // photopic state. P99 catches large bright regions; peak catches tiny
    // sources such as the sun or intense emissive pixels.
    f32 overloadP99StartStops{4.0F};
    f32 overloadPeakStartStops{7.0F};
    f32 overloadSoftRangeStops{4.0F};
    f32 overloadAttackSeconds{0.035F};
    f32 overloadRecoverySeconds{0.10F};
};

struct HumanEyeAdaptationState
{
    bool initialized{false};
    bool ceilingRecoveryActive{false};

    f32 photopicLog2{0.0F};
    f32 darkAdaptation{0.0F};
    f32 overload{0.0F};

    f32 rawPhotopicTargetLog2{0.0F};
    f32 photopicTargetLog2{0.0F};
    f32 photopicCeilingExcessStops{0.0F};

    f32 p95ExcessStops{0.0F};
    f32 p99ExcessStops{0.0F};
    f32 peakExcessStops{0.0F};

    f32 exposureScale{1.0F};
    f32 targetExposureScale{1.0F};

    f32 darkTarget{0.0F};
    f32 overloadTarget{0.0F};
};

using HumanEyeAdaptationUpdateOverride =
    HumanEyeAdaptationState (*)(
        HumanEyeAdaptationState state,
        const LuminanceHistogramStatistics& statistics,
        f32 deltaSeconds,
        const HumanEyeAdaptationConfig& config) noexcept;

// Production call site. In development Studio builds this may dispatch through
// the native hot-reload host; packaged/runtime builds remain direct.
[[nodiscard]] HumanEyeAdaptationState
UpdateHumanEyeAdaptation(
    HumanEyeAdaptationState state,
    const LuminanceHistogramStatistics& statistics,
    f32 deltaSeconds,
    const HumanEyeAdaptationConfig& config = {}) noexcept;

// Stable built-in implementation used as the fallback and by the reloadable
// module itself. Keeping state in the caller allows DLL replacement without
// losing exposure, dark-adaptation, or overload history.
[[nodiscard]] HumanEyeAdaptationState
UpdateHumanEyeAdaptationBuiltin(
    HumanEyeAdaptationState state,
    const LuminanceHistogramStatistics& statistics,
    f32 deltaSeconds,
    const HumanEyeAdaptationConfig& config = {}) noexcept;

void SetHumanEyeAdaptationUpdateOverride(
    HumanEyeAdaptationUpdateOverride update) noexcept;

void ResetHumanEyeAdaptation(
    HumanEyeAdaptationState& state) noexcept;
} // namespace orbit::post_process
