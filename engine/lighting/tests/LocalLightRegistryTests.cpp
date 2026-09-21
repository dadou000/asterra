#include <orbit/lighting/LocalLightRegistry.hpp>

int main()
{
    using namespace orbit::lighting;

    LightingView view;
    view.forward = {0.0F, 0.0F, 1.0F};
    view.up = {0.0F, 1.0F, 0.0F};
    view.verticalFovRadians = 1.22173048F;

    const LocalLight light{
        .type = LocalLightType::Point,
        .positionInFrameMeters = {0.0, 0.0, 5.0},
        .luminousFluxLumens = 1000.0F,
        .rangeMeters = 2.0F,
        .stableId = 42U
    };

    const auto grid =
        BuildTiledLightGrid(
            std::span<const LocalLight>(
                &light,
                1U),
            view,
            128U,
            128U,
            {
                .tileSizePixels = 16U,
                .maximumLightsPerTile = 8U
            });

    if (grid.tilesX != 8U ||
        grid.tilesY != 8U ||
        grid.lights.size() != 1U ||
        grid.offsets.size() != 65U)
    {
        return 1;
    }

    bool referenced = false;
    for (const u32 index : grid.lightIndices)
    {
        if (index == 0U)
        {
            referenced = true;
            break;
        }
    }

    if (!referenced ||
        grid.lights[0].stableId != 42U)
    {
        return 2;
    }

    return 0;
}
