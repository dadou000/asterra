#include <orbit/studio_ui/VolumeSurfaceEffectRenderBridge.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr << "Volume surface effect render bridge test failed.\n";
        std::exit(1);
    }
}
}

int main()
{
    using namespace orbit;
    using namespace orbit::studio_session;
    using namespace orbit::studio_ui;
    using namespace orbit::volume_representation;

    const universe::BodyId bodyA{1U, 2U};
    const universe::BodyId bodyB{3U, 4U};
    constexpr f64 radius = 4'200'000.0;

    const VolumeSurfaceEffectStamp source[]{
        {
            .body = bodyA,
            .effect = VolumeSurfaceEffect::Wetness,
            .bodyLocalSurfacePointMeters = {radius, 0.0, 0.0},
            .radiusMeters = 3.0F,
            .amount = 0.25F
        },
        {
            .body = bodyA,
            .effect = VolumeSurfaceEffect::Heat,
            .bodyLocalSurfacePointMeters = {0.0, radius, 0.0},
            .radiusMeters = 5.0F,
            .amount = 2.0F
        },
        {
            .body = bodyB,
            .effect = VolumeSurfaceEffect::Soot,
            .bodyLocalSurfacePointMeters = {0.0, 0.0, radius},
            .radiusMeters = 4.0F,
            .amount = 8.0F
        }
    };

    const auto batch =
        BuildVolumeSurfaceEffectRenderBatch(
            bodyA,
            radius,
            source,
            8U);

    Check(batch.body == bodyA);
    Check(batch.sourceStampCount == 2U);
    Check(batch.droppedForRenderBudget == 0U);
    Check(batch.stamps.size() == 2U);
    Check(batch.stamps[0].effect == terrain_render::SurfaceEffectKind::Heat);
    Check(batch.stamps[0].amount == 2.0F);
    Check(std::abs(batch.stamps[0].bodyFixedDirection.y - 1.0F) < 1.0e-6F);
    Check(std::abs(batch.stamps[0].angularRadiusRadians - 5.0F / static_cast<f32>(radius)) < 1.0e-10F);
    Check(batch.stamps[1].effect == terrain_render::SurfaceEffectKind::Wetness);
    Check(std::abs(batch.stamps[1].bodyFixedDirection.x - 1.0F) < 1.0e-6F);

    const auto constrained =
        BuildVolumeSurfaceEffectRenderBatch(
            bodyA,
            radius,
            source,
            1U);
    Check(constrained.stamps.size() == 1U);
    Check(constrained.droppedForRenderBudget == 1U);
    Check(constrained.stamps.front().effect == terrain_render::SurfaceEffectKind::Heat);

    return 0;
}
