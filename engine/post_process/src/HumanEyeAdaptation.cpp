#include <orbit/post_process/HumanEyeAdaptation.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace orbit::post_process
{
namespace
{
std::atomic<HumanEyeAdaptationUpdateOverride>
    gUpdateOverride{nullptr};

[[nodiscard]] f32 FiniteOr(
    const f32 value,
    const f32 fallback) noexcept
{
    return std::isfinite(value)
        ? value
        : fallback;
}

[[nodiscard]] f32 ExpApproach(
    const f32 current,
    const f32 target,
    const f32 deltaSeconds,
    const f32 timeConstantSeconds) noexcept
{
    const f32 dt =
        std::max(
            FiniteOr(deltaSeconds, 0.0F),
            0.0F);

    const f32 tau =
        std::max(
            FiniteOr(timeConstantSeconds, 0.001F),
            0.001F);

    const f32 alpha =
        1.0F -
        std::exp(-dt / tau);

    return
        current +
        (target - current) *
            std::clamp(alpha, 0.0F, 1.0F);
}

[[nodiscard]] f32 Saturate(
    const f32 value) noexcept
{
    return std::clamp(value, 0.0F, 1.0F);
}

// A bright region that fills a meaningful share of the frame keeps the eye
// photopic. The frame median alone cannot see it: with 80% of the frame dark
// and 20% fully bright the median is dark, so exposure opened ~10 stops on the
// dark majority (blowing the bright 20% out) and dark adaptation started
// accumulating. p90 is the share proxy -- once p90 is bright, at least ~10% of
// the weighted frame is bright, so the 20% case clears it with margin while a
// handful of bright pixels (p90 still dark) leaves the median in charge.
//
// The eye cannot sit more than this many stops below such a region (veiling
// glare from the bright area washes out the dark detail around it), so the
// metered "median" is floored at p90 minus this span. Scenes whose p90 is
// within the span of the median are untouched.
constexpr f32 kBrightRegionSpanStops = 3.0F;

// "No limit" for an adaptation level in log2 scene luminance.
constexpr f32 kNoLimitLog2 = -64.0F;

// Photometric limits on the exposure, from this frame's statistics:
//  - the brightest metered pixel (peak) and the adaptation level above which
//    the exposure keeps it at or below the display's peak luminance, and
//  - the natural boost floor: the lowest adaptation level the photopic gain can
//    reach, `maximumBoostStops` below the daylight adaptation luminance.
// Luminance is converted to cd/m^2 through config.nitsPerSceneUnit.
void UpdatePhotometricLimits(
    HumanEyeAdaptationState& state,
    const LuminanceHistogramStatistics& statistics,
    const f32 peakLog2,
    const HumanEyeAdaptationConfig& config) noexcept
{
    const f32 nitsPerUnit =
        std::max(
            FiniteOr(
                config.nitsPerSceneUnit,
                kSceneLuminanceNitsPerUnit),
            1.0e-3F);

    // Hand-built statistics may leave the linear peak unset; the log2 value
    // is then authoritative.
    const f32 peakScene =
        std::isfinite(statistics.peakLuminance) &&
                statistics.peakLuminance > 0.0F
            ? statistics.peakLuminance
            : std::exp2(peakLog2);

    state.peakNits =
        peakScene * nitsPerUnit;

    const f32 middleGray =
        std::max(
            FiniteOr(
                config.exposureMiddleGray,
                0.18F),
            1.0e-6F);

    const f32 glareScene =
        std::max(
            FiniteOr(
                config.glareThresholdNits,
                1.0e6F),
            1.0e-3F) /
        nitsPerUnit;
    const f32 protectedScene =
        std::min(
            peakScene,
            glareScene);

    const f32 referenceWhite =
        std::max(
            FiniteOr(
                config.referenceWhiteNits,
                203.0F),
            1.0e-3F);
    const f32 headroom =
        std::max(
            FiniteOr(
                config.highlightTargetNits,
                1000.0F) /
                referenceWhite,
            1.0F);

    // exposure = middleGray / 2^A and the protected pixel must satisfy
    // exposure * protectedScene <= headroom, hence A >= log2(middleGray *
    // protectedScene / headroom).
    state.peakProtectTargetLog2 =
        config.highlightProtection &&
                protectedScene > 0.0F
            ? std::log2(
                  middleGray *
                  protectedScene /
                  headroom)
            : kNoLimitLog2;

    const f32 daylightScene =
        std::max(
            FiniteOr(
                config.daylightAdaptationNits,
                50000.0F),
            1.0e-3F) /
        nitsPerUnit;

    state.minimumAdaptationLog2 =
        std::log2(daylightScene) -
        std::max(
            FiniteOr(
                config.maximumBoostStops,
                6.0F),
            0.0F);
}

// Builds the presentation exposure from the adapted level and the two limits.
// The adapted level is never lower than the natural boost floor and never
// lower than what keeps the brightest protected pixel on screen.
void ApplyExposure(
    HumanEyeAdaptationState& state,
    const HumanEyeAdaptationConfig& config) noexcept
{
    const f32 middleGray =
        std::max(
            FiniteOr(
                config.exposureMiddleGray,
                0.18F),
            1.0e-6F);
    const f32 minimumExposure =
        std::max(
            FiniteOr(
                config.minimumExposureScale,
                1.0F / 4096.0F),
            1.0e-8F);
    const f32 maximumExposure =
        std::max(
            FiniteOr(
                config.maximumExposureScale,
                4096.0F),
            minimumExposure);

    const f32 boostLimited =
        std::max(
            state.photopicLog2,
            state.minimumAdaptationLog2);

    state.exposureAdaptationLog2 =
        std::max(
            boostLimited,
            state.peakProtectLog2);
    state.boostLimitStops =
        std::max(
            state.minimumAdaptationLog2 -
                state.photopicLog2,
            0.0F);
    state.highlightProtectionStops =
        std::max(
            state.peakProtectLog2 -
                boostLimited,
            0.0F);

    state.exposureScale =
        std::clamp(
            middleGray /
                std::exp2(state.exposureAdaptationLog2),
            minimumExposure,
            maximumExposure);

    const f32 targetAdaptation =
        std::max(
            std::max(
                state.photopicTargetLog2,
                state.minimumAdaptationLog2),
            state.peakProtectTargetLog2);

    state.targetExposureScale =
        std::clamp(
            middleGray /
                std::exp2(targetAdaptation),
            minimumExposure,
            maximumExposure);
}
} // namespace

HumanEyeAdaptationState
UpdateHumanEyeAdaptation(
    HumanEyeAdaptationState state,
    const LuminanceHistogramStatistics& statistics,
    const f32 deltaSeconds,
    const HumanEyeAdaptationConfig& config) noexcept
{
    if (const auto update =
            gUpdateOverride.load(
                std::memory_order_acquire);
        update != nullptr)
    {
        return update(
            state,
            statistics,
            deltaSeconds,
            config);
    }

    return UpdateHumanEyeAdaptationBuiltin(
        state,
        statistics,
        deltaSeconds,
        config);
}

HumanEyeAdaptationState
UpdateHumanEyeAdaptationBuiltin(
    HumanEyeAdaptationState state,
    const LuminanceHistogramStatistics& statistics,
    const f32 deltaSeconds,
    const HumanEyeAdaptationConfig& config) noexcept
{
    if (!statistics.valid)
    {
        return state;
    }

    const f32 p50 =
        FiniteOr(
            statistics.medianLog2,
            state.photopicLog2);
    const f32 p95 =
        FiniteOr(
            statistics.p95Log2,
            p50);
    const f32 p99 =
        FiniteOr(
            statistics.p99Log2,
            p95);
    const f32 peak =
        FiniteOr(
            statistics.peakLog2,
            p99);

    // Real percentiles are monotonic; hand-built statistics may leave p90
    // unset (0), so pin it between the median and p95 before using it.
    const f32 p90 =
        std::min(
            std::max(
                FiniteOr(
                    statistics.p90Log2,
                    p50),
                p50),
            std::max(p95, p50));

    // Median the adaptation actually meters: raised toward a large bright
    // region so a dark majority cannot drag exposure or dark adaptation down.
    const f32 meteredMedian =
        std::max(
            p50,
            p90 - kBrightRegionSpanStops);

    const f32 p50Weight =
        Saturate(
            FiniteOr(
                config.photopicP50Weight,
                0.78F));
    const f32 p95Weight =
        std::max(
            FiniteOr(
                config.photopicP95Weight,
                0.22F),
            0.0F);
    const f32 normalization =
        std::max(
            p50Weight + p95Weight,
            1.0e-4F);

    state.rawPhotopicTargetLog2 =
        (meteredMedian * p50Weight +
         p95 * p95Weight) /
        normalization;

    const f32 photopicCeiling =
        FiniteOr(
            config.photopicCeilingLog2,
            2.0F);

    state.photopicTargetLog2 =
        std::min(
            state.rawPhotopicTargetLog2,
            photopicCeiling);

    state.photopicCeilingExcessStops =
        std::max(
            state.rawPhotopicTargetLog2 -
                photopicCeiling,
            0.0F);

    if (state.photopicCeilingExcessStops > 0.0F)
    {
        state.ceilingRecoveryActive = true;
    }

    state.p95ExcessStops =
        std::max(
            p95 - photopicCeiling,
            0.0F);
    state.p99ExcessStops =
        std::max(
            p99 - photopicCeiling,
            0.0F);
    state.peakExcessStops =
        std::max(
            peak - photopicCeiling,
            0.0F);

    UpdatePhotometricLimits(
        state,
        statistics,
        peak,
        config);

    const f32 darkThreshold =
        FiniteOr(
            config.darkThresholdLog2,
            -3.0F);
    const f32 darkFull =
        std::min(
            FiniteOr(
                config.darkFullLog2,
                -9.0F),
            darkThreshold - 0.01F);

    state.darkTarget =
        Saturate(
            (darkThreshold - meteredMedian) /
            (darkThreshold - darkFull));

    const f32 adaptedReference =
        state.initialized
            ? state.photopicLog2
            : state.photopicTargetLog2;

    const f32 overloadRange =
        std::max(
            FiniteOr(
                config.overloadSoftRangeStops,
                4.0F),
            0.01F);

    const f32 p99Over =
        (p99 - adaptedReference) -
        FiniteOr(
            config.overloadP99StartStops,
            4.0F);
    const f32 peakOver =
        (peak - adaptedReference) -
        FiniteOr(
            config.overloadPeakStartStops,
            7.0F);

    state.overloadTarget =
        Saturate(
            std::max(
                p99Over,
                peakOver) /
            overloadRange);

    if (!state.initialized)
    {
        state.initialized = true;
        state.photopicLog2 =
            state.photopicTargetLog2;
        state.darkAdaptation =
            state.darkTarget;
        state.overload =
            state.overloadTarget;

        state.peakProtectLog2 =
            state.peakProtectTargetLog2;

        ApplyExposure(
            state,
            config);
        return state;
    }

    const bool releasingCeilingOverload =
        state.ceilingRecoveryActive &&
        state.photopicCeilingExcessStops <= 0.0F &&
        state.photopicTargetLog2 <
            state.photopicLog2;

    state.photopicLog2 =
        ExpApproach(
            state.photopicLog2,
            state.photopicTargetLog2,
            deltaSeconds,
            state.photopicTargetLog2 >
                    state.photopicLog2
                ? config.photopicBrightenSeconds
                : (releasingCeilingOverload
                    ? config.photopicCeilingRecoverySeconds
                    : config.photopicDarkenSeconds));

    if (state.ceilingRecoveryActive &&
        state.photopicCeilingExcessStops <= 0.0F &&
        std::abs(
            state.photopicLog2 -
            state.photopicTargetLog2) <= 0.05F)
    {
        state.ceilingRecoveryActive = false;
    }

    state.darkAdaptation =
        ExpApproach(
            state.darkAdaptation,
            state.darkTarget,
            deltaSeconds,
            state.darkTarget >
                    state.darkAdaptation
                ? config.darkAdaptSeconds
                : config.darkResetSeconds);

    state.overload =
        ExpApproach(
            state.overload,
            state.overloadTarget,
            deltaSeconds,
            state.overloadTarget >
                    state.overload
                ? config.overloadAttackSeconds
                : config.overloadRecoverySeconds);

    state.darkAdaptation =
        Saturate(state.darkAdaptation);
    state.overload =
        Saturate(state.overload);

    // Protecting the highlights is a fast reflex (the pupil closes at once when
    // something bright appears); letting go follows the normal darkening time.
    state.peakProtectLog2 =
        ExpApproach(
            state.peakProtectLog2,
            state.peakProtectTargetLog2,
            deltaSeconds,
            state.peakProtectTargetLog2 >
                    state.peakProtectLog2
                ? config.highlightAttackSeconds
                : config.photopicDarkenSeconds);

    ApplyExposure(
        state,
        config);

    return state;
}

void SetHumanEyeAdaptationUpdateOverride(
    const HumanEyeAdaptationUpdateOverride update) noexcept
{
    gUpdateOverride.store(
        update,
        std::memory_order_release);
}

void ResetHumanEyeAdaptation(
    HumanEyeAdaptationState& state) noexcept
{
    state = {};
}
} // namespace orbit::post_process
