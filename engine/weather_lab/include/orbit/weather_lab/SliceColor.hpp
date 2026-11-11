#pragma once

#include <orbit/weather_lab/WeatherLabSession.hpp>

#include <algorithm>
#include <array>
#include <cmath>

// Colour mapping for slice display, kept out of the UI so the ramps are
// testable and the panel and any future GPU renderer agree.

namespace orbit::weather_lab
{
// Fields that change sign are drawn on a diverging ramp centred on zero.
[[nodiscard]] inline bool IsSignedField(const std::string& field)
{
    return field == "w" || field == "thp" || field == "zvort";
}

namespace detail
{
[[nodiscard]] inline std::array<float, 4> Lerp(
    const std::array<float, 4>& a, const std::array<float, 4>& b, const float t)
{
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t,
            a[2] + (b[2] - a[2]) * t, 1.0F};
}

// Piecewise-linear ramp through `stops` (at least two, evenly spaced).
template <std::size_t N>
[[nodiscard]] std::array<float, 4> Ramp(
    const std::array<std::array<float, 4>, N>& stops, float t)
{
    t = std::clamp(t, 0.0F, 1.0F) * static_cast<float>(N - 1U);
    const auto i = std::min(static_cast<std::size_t>(t), N - 2U);
    return Lerp(stops[i], stops[i + 1U], t - static_cast<float>(i));
}
} // namespace detail

// RGBA (alpha 1) for `value` in `slice`. Sequential fields run dark blue to
// yellow over [min, max]; signed fields run blue-white-red over [-m, m] with
// m = max(|min|, |max|), so zero is always white.
[[nodiscard]] inline std::array<float, 4> SliceColor(
    const Slice& slice, const float value)
{
    if (IsSignedField(slice.field))
    {
        static constexpr std::array<std::array<float, 4>, 5> kDiverging{{
            {0.10F, 0.20F, 0.62F, 1.0F}, {0.42F, 0.62F, 0.90F, 1.0F},
            {0.96F, 0.96F, 0.96F, 1.0F}, {0.93F, 0.58F, 0.40F, 1.0F},
            {0.66F, 0.10F, 0.12F, 1.0F}}};
        const float m = std::max(
            std::max(std::fabs(slice.minValue), std::fabs(slice.maxValue)), 1.0e-9F);
        return detail::Ramp(kDiverging, 0.5F + 0.5F * value / m);
    }
    static constexpr std::array<std::array<float, 4>, 5> kSequential{{
        {0.05F, 0.06F, 0.20F, 1.0F}, {0.20F, 0.20F, 0.52F, 1.0F},
        {0.18F, 0.52F, 0.60F, 1.0F}, {0.45F, 0.78F, 0.38F, 1.0F},
        {0.98F, 0.90F, 0.25F, 1.0F}}};
    const float range = std::max(slice.maxValue - slice.minValue, 1.0e-9F);
    return detail::Ramp(kSequential, (value - slice.minValue) / range);
}
} // namespace orbit::weather_lab
