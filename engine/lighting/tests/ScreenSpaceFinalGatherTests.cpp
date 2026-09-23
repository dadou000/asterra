#include <orbit/lighting/ScreenSpaceFinalGather.hpp>

int main()
{
    using namespace orbit;
    using namespace orbit::lighting;

    static_assert(requires(
        ScreenSpaceFinalGatherRenderer& renderer,
        rhi::CommandList& commands,
        rhi::Texture& sceneColor,
        rhi::Texture& baseRoughness,
        rhi::Texture& normalMetallic,
        rhi::Texture& emissionClass,
        rhi::Texture& depth,
        rhi::Texture& previousIndirect,
        rhi::Texture& previousMeta,
        rhi::Texture& currentIndirect,
        rhi::Texture& currentMeta,
        const LightingView& view,
        rhi::Buffer& particleLightGrid)
    {
        renderer.Gather(
            commands,
            sceneColor,
            baseRoughness,
            normalMetallic,
            emissionClass,
            depth,
            previousIndirect,
            previousMeta,
            currentIndirect,
            currentMeta,
            1920U,
            1080U,
            view,
            false,
            &particleLightGrid,
            ScreenSpaceFinalGatherSettings{});
    });

    LightingView previous;
    previous.frame = frames::FrameId{
        .high = 1U,
        .low = 2U};
    previous.body = universe::BodyId{
        .high = 3U,
        .low = 4U};
    previous.cameraPositionInFrameMeters =
        {10.0, 20.0, 30.0};
    previous.forward =
        {0.0F, 0.0F, 1.0F};
    previous.up =
        {0.0F, 1.0F, 0.0F};

    LightingView current = previous;

    if (!CanReuseFinalGatherHistory(
            previous,
            current))
    {
        return 1;
    }

    current.cameraPositionInFrameMeters.x +=
        0.01;

    if (CanReuseFinalGatherHistory(
            previous,
            current))
    {
        return 2;
    }

    current = previous;
    current.change =
        LightingViewChange::CameraCut;

    if (CanReuseFinalGatherHistory(
            previous,
            current))
    {
        return 3;
    }

    current = previous;
    current.body = universe::BodyId{
        .high = 5U,
        .low = 6U};

    if (CanReuseFinalGatherHistory(
            previous,
            current))
    {
        return 4;
    }

    current = previous;
    current.forward =
        {0.01F, 0.0F, 0.99995F};

    if (CanReuseFinalGatherHistory(
            previous,
            current))
    {
        return 5;
    }

    return 0;
}