#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/post_process/LuminanceHistogram.hpp>

namespace orbit::post_process
{
// Photometric calibration of the HDR scene target. Luminance in this module is
// expressed in candela per square metre (nits). The lighting passes divide
// irradiance by the 1361 W/m^2 solar reference, so a scene-linear 1.0 is a
// radiance of 1361 W/(m^2 sr); at the 683 lm/W peak photopic luminous efficacy
// those passes use for lights, that is 929,563 cd/m^2. Every nit or stop value
// below converts through this one factor.
inline constexpr f32 kSolarReferenceIrradianceWattsPerSquareMeter = 1361.0F;
inline constexpr f32 kPhotopicLuminousEfficacyLumensPerWatt = 683.0F;
inline constexpr f32 kSceneLuminanceNitsPerUnit =
    kSolarReferenceIrradianceWattsPerSquareMeter *
    kPhotopicLuminousEfficacyLumensPerWatt;

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

    // Candela per square metre of one scene-linear unit (see above).
    f32 nitsPerSceneUnit{kSceneLuminanceNitsPerUnit};

    // Highlight protection. The eye never opens so far that the brightest
    // metered pixel lands above what the display can show: exposure is capped
    // so that pixel maps to `highlightTargetNits` (the display's peak). Sources
    // brighter than `glareThresholdNits` (the sun disc, specular glints; no
    // diffusely lit surface reaches them) are glare, not protected: they may
    // saturate, as they do for a real eye, and feed the overload response.
    // `referenceWhiteNits` is the display luminance of an exposed value of 1.0;
    // both display values are copied from the tone-mapping config by the
    // renderer so there is one source of truth.
    bool highlightProtection{true};
    f32 referenceWhiteNits{203.0F};
    f32 highlightTargetNits{1000.0F};
    f32 glareThresholdNits{1.0e6F};
    f32 highlightAttackSeconds{0.04F};

    // Natural boost limit. Photopic gain can raise sensitivity only so far
    // above the setting for full daylight: `daylightAdaptationNits` is the
    // adaptation luminance of a sunlit scene (about a sunlit mid-gray surface
    // in this engine's photometry) and the gain may exceed that setting by at
    // most `maximumBoostStops`. Darker scenes then simply look dark, as they do
    // before the slow dark-adaptation mechanism (below) has had time to act.
    f32 daylightAdaptationNits{50000.0F};
    f32 maximumBoostStops{6.0F};

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

    // Photometric diagnostics and the two limits on the exposure above.
    // Adaptation levels are log2 scene-linear luminance, like photopicLog2.
    f32 peakNits{0.0F};                    // brightest metered pixel, cd/m^2
    f32 peakProtectTargetLog2{-64.0F};     // adaptation that keeps it on screen
    f32 peakProtectLog2{-64.0F};           // the same, smoothed
    f32 minimumAdaptationLog2{-64.0F};     // natural boost limit
    f32 exposureAdaptationLog2{0.0F};      // level the exposure is built from
    f32 highlightProtectionStops{0.0F};    // exposure removed by protection
    f32 boostLimitStops{0.0F};             // boost the limit refused
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
