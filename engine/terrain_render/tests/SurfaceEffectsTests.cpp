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
            .bodyLocalPointMeters = {10.0F, 0.0F, 0.0F},
            .radiusMeters = 4.0F,
            .amount = 0.8F,
            .effect = SurfaceEffectKind::Wetness
        },
        {
            .bodyLocalPointMeters = {10.0F, 0.0F, 0.0F},
            .radiusMeters = 2.0F,
            .amount = 0.5F,
            .effect = SurfaceEffectKind::Soot
        },
        {
            .bodyLocalPointMeters = {10.0F, 0.0F, 0.0F},
            .radiusMeters = 3.0F,
            .amount = 2.0F,
            .effect = SurfaceEffectKind::Heat
        }
    };

    const auto center =
        EvaluateSurfaceEffects(
            {10.0F, 0.0F, 0.0F},
            stamps);

    Check(std::abs(center.wetness - 0.8F) < 1.0e-6F);
    Check(std::abs(center.soot - 0.5F) < 1.0e-6F);
    Check(std::abs(center.heat - 2.0F) < 1.0e-6F);

    const auto edge =
        EvaluateSurfaceEffects(
            {14.0F, 0.0F, 0.0F},
            stamps);
    Check(edge.wetness == 0.0F);
    Check(edge.soot == 0.0F);
    Check(edge.heat == 0.0F);

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
