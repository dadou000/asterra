#include <orbit/terrain_render/SurfaceEffects.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr << "Surface effects test failed.\n";
        std::exit(1);
    }
}
}

int main()
{
    using namespace orbit;
    using namespace orbit::terrain_render;

    const SurfaceEffectGpuStamp stamps[]{
        {
            .bodyFixedDirection = {1.0F, 0.0F, 0.0F},
            .angularRadiusRadians = 0.04F,
            .amount = 0.8F,
            .effect = SurfaceEffectKind::Wetness
        },
        {
            .bodyFixedDirection = {1.0F, 0.0F, 0.0F},
            .angularRadiusRadians = 0.02F,
            .amount = 0.5F,
            .effect = SurfaceEffectKind::Soot
        },
        {
            .bodyFixedDirection = {1.0F, 0.0F, 0.0F},
            .angularRadiusRadians = 0.03F,
            .amount = 2.0F,
            .effect = SurfaceEffectKind::Heat
        }
    };

    const auto center =
        EvaluateSurfaceEffects(
            {1.0F, 0.0F, 0.0F},
            stamps);

    Check(std::abs(center.wetness - 0.8F) < 1.0e-6F);
    Check(std::abs(center.soot - 0.5F) < 1.0e-6F);
    Check(std::abs(center.heat - 2.0F) < 1.0e-6F);

    const auto outside =
        EvaluateSurfaceEffects(
            {0.99F, 0.0F, 0.141067F},
            stamps);
    Check(outside.wetness == 0.0F);
    Check(outside.soot == 0.0F);
    Check(outside.heat == 0.0F);

    const SurfacePbrState base{
        .baseColor = {0.6F, 0.5F, 0.4F},
        .roughness = 0.8F,
        .metallic = 0.2F,
        .emission = {}
    };

    const auto affected = ApplySurfaceEffects(base, center);
    Check(affected.roughness < base.roughness);
    Check(affected.baseColor.x < base.baseColor.x);
    Check(affected.metallic < base.metallic);
    Check(affected.emission.x > 0.0F);
    Check(affected.emission.y > 0.0F);

    const SurfaceEffectInfluence ashOnly{
        .ash = 1.0F
    };
    const auto ash = ApplySurfaceEffects(base, ashOnly);
    Check(ash.roughness > base.roughness);
    Check(std::abs(ash.baseColor.x - 0.43F) < 1.0e-6F);

    const SurfaceEffectInfluence sedimentOnly{
        .sediment = 1.0F
    };
    const auto sediment = ApplySurfaceEffects(base, sedimentOnly);
    Check(std::abs(sediment.baseColor.x - 0.36F) < 1.0e-6F);

    return 0;
}
