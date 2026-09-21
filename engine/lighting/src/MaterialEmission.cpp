#include <orbit/lighting/MaterialEmission.hpp>

#include <algorithm>
#include <cmath>

namespace orbit::lighting
{
EvaluatedMaterialEmission
EvaluateMaterialEmission(
    const PhysicalMaterialEmission& emission,
    const f32 photopicLuminousEfficacy) noexcept
{
    const f32 efficacy =
        std::isfinite(photopicLuminousEfficacy)
            ? std::max(
                  photopicLuminousEfficacy,
                  1.0F)
            : 683.0F;

    const f32 luminance =
        std::isfinite(emission.luminanceNits)
            ? std::max(
                  emission.luminanceNits,
                  0.0F)
            : 0.0F;

    const auto channel =
        [](const f32 value) noexcept
        {
            return
                std::isfinite(value)
                    ? std::max(value, 0.0F)
                    : 0.0F;
        };

    const f32 radianceScale =
        luminance /
        efficacy;

    const math::Float3 visible{
        channel(emission.colorLinear.x) *
            radianceScale,
        channel(emission.colorLinear.y) *
            radianceScale,
        channel(emission.colorLinear.z) *
            radianceScale
    };

    const f32 giScale =
        emission.contributesToGi &&
        std::isfinite(emission.giScale)
            ? std::max(
                  emission.giScale,
                  0.0F)
            : 0.0F;

    return {
        .visibleRadiance = visible,
        .giRadiance = {
            visible.x * giScale,
            visible.y * giScale,
            visible.z * giScale
        }
    };
}
} // namespace orbit::lighting
