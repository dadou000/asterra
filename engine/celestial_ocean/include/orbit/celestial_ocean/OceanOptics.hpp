#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

namespace orbit::celestial_ocean
{
struct OceanOpticalParameters
{
    f64 refractiveIndex{1.333};
    f64 orbitalRoughness{0.12};
    math::Double3 absorptionPerMeter{0.18, 0.055, 0.025};
    math::Double3 deepWaterColor{0.008, 0.035, 0.075};
    f64 glintStrength{1.0};
    f64 deepColorDepthMeters{40.0};
};

[[nodiscard]] f64 DielectricNormalReflectance(
    f64 refractiveIndexOutside,
    f64 refractiveIndexInside);

[[nodiscard]] f64 SchlickFresnel(
    f64 cosineTheta,
    f64 normalReflectance);

[[nodiscard]] f64 GgxSpecularBrdf(
    f64 nDotL,
    f64 nDotV,
    f64 nDotH,
    f64 vDotH,
    f64 roughness,
    f64 normalReflectance);

[[nodiscard]] math::Double3 WaterColumnTransmittance(
    const math::Double3& absorptionPerMeter,
    f64 depthMeters);

[[nodiscard]] math::Double3 DeepWaterColor(
    const OceanOpticalParameters& parameters,
    f64 waterDepthMeters);

[[nodiscard]] u64 OceanOpticalFingerprint(
    const OceanOpticalParameters& parameters);
} // namespace orbit::celestial_ocean
