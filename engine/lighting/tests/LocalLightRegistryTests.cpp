#include <orbit/lighting/LocalLightRegistry.hpp>

#include <cmath>
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

    // Energy listed for one tile: individual lights plus proxies (a proxy carries
    // the summed flux of the lights it replaced).
    const auto tileFlux = [](const TiledLightGrid& grid, const orbit::u32 tile)
    {
        double sum = 0.0;
        for (orbit::u32 i = grid.offsets[tile]; i < grid.offsets[tile + 1U]; ++i)
        {
            sum += grid.lights[grid.lightIndices[i]].luminousFluxLumens;
        }
        return sum;
    };

    // Many overlapping lights with a small per-tile cap: strongest-first (equal
    // scores keep submission order), the last slot of an overflowing tile holds
    // one proxy with the dropped flux; offsets stay a consistent prefix sum.
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
        capped.proxyLights != 1U ||
        capped.lights.size() != crowd.size() + 1U ||
        (capped.lights.back().stableId & kProxyLightIdBit) == 0U)
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
        if (end - begin != 3U ||
            capped.lightIndices[begin] != 0U ||
            capped.lightIndices[begin + 1U] != 1U ||
            capped.lightIndices[begin + 2U] != capped.lights.size() - 1U)
        {
            return 5;
        }
        if (std::abs(tileFlux(capped, tile) - 10000.0) > 1.0)
        {
            return 6;   // flux must be conserved, not truncated
        }
    }

    if (coveredTiles == 0U ||
        capped.droppedAssignments != coveredTiles * 8U)
    {
        return 7;
    }

    // The old policy kept the first lights in submission order, so a strong
    // light submitted after a crowd of weak ones vanished. It must now survive
    // in every tile it covers, and the total flux must still be conserved.
    std::vector<LocalLight> crowdThenStrong(200U, light);
    for (std::size_t i = 0U; i < crowdThenStrong.size(); ++i)
    {
        crowdThenStrong[i].stableId = 1000U + i;
        crowdThenStrong[i].luminousFluxLumens = 1.0F;
    }
    LocalLight strong = light;
    strong.stableId = 7U;
    strong.luminousFluxLumens = 50000.0F;
    crowdThenStrong.push_back(strong);

    const auto mixed =
        BuildTiledLightGrid(
            crowdThenStrong,
            view,
            128U,
            128U,
            {
                .tileSizePixels = 16U,
                .maximumLightsPerTile = 8U
            });

    for (orbit::u32 tile = 0U; tile < 64U; ++tile)
    {
        const orbit::u32 begin = mixed.offsets[tile];
        const orbit::u32 end = mixed.offsets[tile + 1U];
        if (end == begin)
        {
            continue;
        }
        bool hasStrong = false;
        for (orbit::u32 i = begin; i < end; ++i)
        {
            hasStrong = hasStrong ||
                mixed.lights[mixed.lightIndices[i]].stableId == 7U;
        }
        if (!hasStrong)
        {
            return 8;
        }
        if (std::abs(tileFlux(mixed, tile) - 50200.0) > 1.0)
        {
            return 9;
        }
    }

    // aggregateOverflow = false keeps the legacy first-N truncation.
    const auto legacy =
        BuildTiledLightGrid(
            crowd,
            view,
            128U,
            128U,
            {
                .tileSizePixels = 16U,
                .maximumLightsPerTile = 3U,
                .aggregateOverflow = false
            });
    if (legacy.proxyLights != 0U ||
        legacy.lights.size() != crowd.size() ||
        legacy.droppedAssignments != coveredTiles * 7U)
    {
        return 10;
    }

    return 0;
}
