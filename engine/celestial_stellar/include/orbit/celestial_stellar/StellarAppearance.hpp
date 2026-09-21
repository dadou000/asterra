#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

namespace orbit::celestial_stellar
{
struct StellarAppearanceParameters
{
    f64 effectiveTemperatureKelvin{5772.0};
    f64 limbDarkening{0.58};
    f64 granulationStrength{0.10};
    f64 granulationScale{42.0};
    f64 activityLevel{0.12};
    u64 activitySeed{1};
    f64 chromosphereStrength{0.08};
    f64 chromosphereExtent{0.035};
    f64 coronaStrength{0.025};
    f64 coronaExtent{1.75};
    f64 glareStrength{0.35};
    f64 glareRadiusPixels{5.0};
};

[[nodiscard]] math::Double3 BlackbodyColorLinear(
    f64 temperatureKelvin);

[[nodiscard]] f64 LinearLimbDarkening(
    f64 cosineEmissionAngle,
    f64 coefficient);

[[nodiscard]] f64 GranulationModulation(
    math::Double3 unitSurfaceDirection,
    const StellarAppearanceParameters& parameters) noexcept;

[[nodiscard]] f64 ActivityModulation(
    math::Double3 unitSurfaceDirection,
    const StellarAppearanceParameters& parameters) noexcept;

[[nodiscard]] f64 ChromosphereProfile(
    f64 normalizedRadius,
    const StellarAppearanceParameters& parameters) noexcept;

[[nodiscard]] f64 CoronaProfile(
    f64 normalizedRadius,
    const StellarAppearanceParameters& parameters) noexcept;

[[nodiscard]] u64 StellarAppearanceFingerprint(
    const StellarAppearanceParameters& parameters);
} // namespace orbit::celestial_stellar
