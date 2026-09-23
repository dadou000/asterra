#include <orbit/studio_ui/VolumeSurfaceEffectRenderBridge.hpp>

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

    const VolumeSurfaceEffectStamp source[]{
        {
            .body = bodyA,
            .effect = VolumeSurfaceEffect::Wetness,
            .bodyLocalSurfacePointMeters = {10.0, 20.0, 30.0},
            .radiusMeters = 3.0F,
            .amount = 0.25F
        },
        {
            .body = bodyA,
            .effect = VolumeSurfaceEffect::Heat,
            .bodyLocalSurfacePointMeters = {40.0, 50.0, 60.0},
            .radiusMeters = 5.0F,
            .amount = 2.0F
        },
        {
            .body = bodyB,
            .effect = VolumeSurfaceEffect::Soot,
            .bodyLocalSurfacePointMeters = {70.0, 80.0, 90.0},
            .radiusMeters = 4.0F,
            .amount = 8.0F
        }
    };

    const auto batch =
        BuildVolumeSurfaceEffectRenderBatch(
            bodyA,
            source,
            8U);

    Check(batch.body == bodyA);
    Check(batch.sourceStampCount == 2U);
    Check(batch.droppedForRenderBudget == 0U);
    Check(batch.stamps.size() == 2U);
    Check(batch.stamps[0].effect == terrain_render::SurfaceEffectKind::Heat);
    Check(batch.stamps[0].amount == 2.0F);
    Check(batch.stamps[1].effect == terrain_render::SurfaceEffectKind::Wetness);

    const auto constrained =
        BuildVolumeSurfaceEffectRenderBatch(
            bodyA,
            source,
            1U);
    Check(constrained.stamps.size() == 1U);
    Check(constrained.droppedForRenderBudget == 1U);
    Check(constrained.stamps.front().effect == terrain_render::SurfaceEffectKind::Heat);

    return 0;
}
