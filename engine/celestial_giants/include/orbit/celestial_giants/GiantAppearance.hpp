#pragma once

#include <orbit/celestial_appearance/PlanetaryAppearance.hpp>
#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

#include <string>

namespace orbit::celestial_giants
{
enum class GiantClass : u8
{
    GasGiant = 0,
    IceGiant = 1
};

struct GiantAppearanceParameters
{
    GiantClass giantClass{GiantClass::GasGiant};
    u64 seed{1};
    math::Double3 baseColorLinear{0.62, 0.48, 0.31};
    math::Double3 bandColorLinear{0.90, 0.78, 0.58};
    math::Double3 polarColorLinear{0.48, 0.42, 0.36};
    f64 bandFrequency{11.0};
    f64 bandStrength{0.72};
    f64 zonalShear{0.18};
    f64 stormStrength{0.35};
    f64 stormScale{5.0};
    f64 polarStrength{0.22};
    f64 depthContrast{0.25};
    f64 turbulenceStrength{0.18};
    f64 turbulenceScale{18.0};
};

struct GiantAppearanceConfig
{
    u32 faceResolution{65};
};

[[nodiscard]] u64 GiantAppearanceFingerprint(
    const GiantAppearanceParameters& parameters,
    const GiantAppearanceConfig& config = {});

[[nodiscard]] celestial_appearance::PlanetaryAppearanceProduct
BuildGiantAppearance(
    const GiantAppearanceParameters& parameters,
    const GiantAppearanceConfig& config = {});

[[nodiscard]] math::Double3 EvaluateGiantColor(
    const GiantAppearanceParameters& parameters,
    math::Double3 unitDirection) noexcept;

} // namespace orbit::celestial_giants
