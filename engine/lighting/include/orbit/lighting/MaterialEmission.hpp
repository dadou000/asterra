#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

namespace orbit::lighting
{
struct PhysicalMaterialEmission
{
    math::Float3 colorLinear{1.0F, 1.0F, 1.0F};
    f32 luminanceNits{0.0F};
    bool contributesToGi{true};
    f32 giScale{1.0F};
};

struct EvaluatedMaterialEmission
{
    // Physical radiance approximation in W/(sr*m^2), represented in the
    // engine's linear RGB channels. This remains HDR and is never display
    // clamped here.
    math::Float3 visibleRadiance{};

    // Same physical source, independently gated/scaled for GI transport.
    math::Float3 giRadiance{};
};

[[nodiscard]] EvaluatedMaterialEmission
EvaluateMaterialEmission(
    const PhysicalMaterialEmission& emission,
    f32 photopicLuminousEfficacy = 683.0F) noexcept;
} // namespace orbit::lighting
