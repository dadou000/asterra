#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

namespace orbit::lighting
{
struct PhysicalEmissionMaterial
{
    // Linear RGB chromaticity/modulation. Brightness authority is luminanceNits.
    math::Float3 colorLinear{1.0F, 1.0F, 1.0F};

    // Authored photometric surface luminance in cd/m^2 (nits).
    f32 luminanceNits{0.0F};

    // GI policy is deliberately independent from visible emission.
    bool contributesToGi{true};
    f32 giScale{1.0F};
};

struct PhysicalEmissionEvaluation
{
    math::Float3 visibleRadianceSceneLinear{};
    math::Float3 giRadianceSceneLinear{};
};

// Evaluates physical material emission into Orbit's normalized scene-radiance
// domain. Color and texture define chromatic/spatial modulation. The combined
// RGB is normalized to unit Rec.709 photopic luminance before applying the
// authored cd/m^2 value so changing hue does not silently change the nit value.
[[nodiscard]] PhysicalEmissionEvaluation EvaluatePhysicalEmission(
    const PhysicalEmissionMaterial& material,
    math::Float3 emissiveTextureLinear = {1.0F, 1.0F, 1.0F},
    f32 photopicLuminousEfficacy = 683.0F,
    f32 solarReferenceIrradiance = 1361.0F) noexcept;
} // namespace orbit::lighting
