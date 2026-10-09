#include <orbit/lighting/LocalLightRegistry.hpp>

#include <vector>

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
    for (const orbit::u32 index : grid.lightIndices)
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

    // Many overlapping lights with a small per-tile cap: each covered tile
    // keeps the first lights in submission order and the rest are counted as
    // dropped; offsets stay a consistent prefix sum.
    std::vector<LocalLight> crowd(10U, light);
    for (std::size_t i = 0U; i < crowd.size(); ++i)
    {
        crowd[i].stableId = 100U + i;
    }

    const auto capped =
        BuildTiledLightGrid(
            crowd,
            view,
            128U,
            128U,
            {
                .tileSizePixels = 16U,
                .maximumLightsPerTile = 3U
            });

    if (capped.offsets.size() != 65U ||
        capped.offsets.front() != 0U ||
        capped.offsets.back() != capped.lightIndices.size() ||
        capped.lights.size() != crowd.size())
    {
        return 3;
    }

    orbit::u32 coveredTiles = 0U;
    for (orbit::u32 tile = 0U; tile < 64U; ++tile)
    {
        const orbit::u32 begin = capped.offsets[tile];
        const orbit::u32 end = capped.offsets[tile + 1U];
        if (end < begin || end - begin > 3U)
        {
            return 4;
        }
        if (end == begin)
        {
            continue;
        }
        ++coveredTiles;
        for (orbit::u32 slot = 0U; slot < end - begin; ++slot)
        {
            if (capped.lightIndices[begin + slot] != slot)
            {
                return 5;
            }
        }
    }

    if (coveredTiles == 0U ||
        capped.droppedAssignments != coveredTiles * 7U)
    {
        return 6;
    }

    return 0;
}
