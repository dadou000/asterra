#pragma once

#include <orbit/celestial_appearance/PlanetaryAppearance.hpp>
#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

#include <vector>

namespace orbit::celestial_small_bodies
{
enum class SmallBodyClass : u8
{
    Asteroid = 0,
    CometNucleus = 1,
    Moonlet = 2
};

struct SmallBodyParameters
{
    SmallBodyClass bodyClass{SmallBodyClass::Asteroid};
    u64 seed{1};

    // Multiplies the authored Reference Shape axes. This is presentation
    // detail, not a replacement for the semantic reference shape.
    math::Double3 axisScale{1.0, 0.82, 0.68};
    f64 irregularity{0.18};
    f64 largeLobeStrength{0.12};
    f64 surfaceRoughness{0.92};

    math::Double3 regolithColorLinear{0.16, 0.145, 0.13};
    math::Double3 freshMaterialColorLinear{0.24, 0.22, 0.19};
    f64 colorVariation{0.18};

    f64 craterDensity{0.55};
    f64 craterDepth{0.12};
    f64 craterRimStrength{0.08};

    // Hapke-style baseline terms used by the far renderer. The model is kept
    // compact and deterministic; these values are authorable rather than
    // hidden constants.
    f64 oppositionStrength{0.55};
    f64 oppositionWidthRadians{0.055};
    f64 singleScatteringAlbedo{0.16};
    f64 macroscopicRoughnessRadians{0.42};
};

struct SmallBodyAppearanceConfig
{
    u32 faceResolution{65};
};

struct SmallBodyShapeProduct
{
    u32 faceResolution{0};
    u64 fingerprint{0};
    f64 minimumRadiusScale{1.0};
    f64 maximumRadiusScale{1.0};
    std::vector<f32> radiusScale;

    [[nodiscard]] f32 At(
        u32 face,
        u32 x,
        u32 y) const;
};

struct RoughSurfacePhotometryInput
{
    math::Double3 normal{0.0, 0.0, 1.0};
    math::Double3 lightDirection{0.0, 0.0, 1.0};
    math::Double3 viewDirection{0.0, 0.0, 1.0};
};

[[nodiscard]] u64 SmallBodyAppearanceFingerprint(
    const SmallBodyParameters& parameters,
    const SmallBodyAppearanceConfig& config = {});

[[nodiscard]] f64 EvaluateRadiusScale(
    const SmallBodyParameters& parameters,
    math::Double3 unitDirection) noexcept;

[[nodiscard]] celestial_appearance::PlanetaryAppearanceProduct
BuildSmallBodyAppearance(
    const SmallBodyParameters& parameters,
    const SmallBodyAppearanceConfig& config = {});

[[nodiscard]] SmallBodyShapeProduct
BuildSmallBodyShape(
    const SmallBodyParameters& parameters,
    const SmallBodyAppearanceConfig& config = {});

[[nodiscard]] f64 EvaluateRoughSurfacePhotometry(
    const SmallBodyParameters& parameters,
    const RoughSurfacePhotometryInput& input) noexcept;

} // namespace orbit::celestial_small_bodies
