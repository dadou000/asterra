#include <orbit/post_process/ToneMapping.hpp>

#include <algorithm>
#include <cmath>

namespace orbit::post_process
{
namespace
{
[[nodiscard]] f32 FiniteOr(
    const f32 value,
    const f32 fallback) noexcept
{
    return std::isfinite(value)
        ? value
        : fallback;
}
} // namespace

f32 DisplayHeadroomRatio(
    const ToneMappingConfig& config) noexcept
{
    const f32 white =
        std::max(
            FiniteOr(
                config.referenceWhiteNits,
                203.0F),
            1.0e-3F);

    const f32 peak =
        std::max(
            FiniteOr(
                config.peakNits,
                1000.0F),
            white);

    return peak / white;
}

f32 ToneMapLuminance(
    const f32 exposedLuminance,
    const ToneMappingConfig& config) noexcept
{
    const f32 x =
        std::max(
            FiniteOr(exposedLuminance, 0.0F),
            0.0F);

    if (!config.enabled)
    {
        return x;
    }

    const f32 headroom =
        DisplayHeadroomRatio(config);

    const f32 shoulderStart =
        std::clamp(
            FiniteOr(
                config.shoulderStart,
                1.0F),
            0.0F,
            std::max(headroom - 1.0e-4F, 0.0F));

    if (x <= shoulderStart ||
        headroom <= shoulderStart + 1.0e-4F)
    {
        return std::min(x, headroom);
    }

    const f32 remaining =
        headroom - shoulderStart;

    const f32 strength =
        std::max(
            FiniteOr(
                config.shoulderStrength,
                1.0F),
            1.0e-3F);

    const f32 scale =
        std::max(
            remaining * strength,
            1.0e-4F);

    const f32 distance =
        x - shoulderStart;

    const f32 mapped =
        shoulderStart +
        remaining *
            (1.0F -
             std::exp(-distance / scale));

    return std::clamp(
        mapped,
        0.0F,
        headroom);
}

ToneMappingDiagnostics
EvaluateToneMappingDiagnostics(
    const ToneMappingConfig& config) noexcept
{
    ToneMappingDiagnostics result{};
    result.headroomRatio =
        DisplayHeadroomRatio(config);
    result.shoulderStart =
        std::clamp(
            config.shoulderStart,
            0.0F,
            result.headroomRatio);
    result.mappedReferenceWhite =
        ToneMapLuminance(
            1.0F,
            config);
    result.mappedPeakProbe =
        ToneMapLuminance(
            result.headroomRatio,
            config);
    return result;
}
} // namespace orbit::post_process
