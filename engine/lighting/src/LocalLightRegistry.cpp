#include <orbit/lighting/LocalLightRegistry.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace orbit::lighting
{
namespace
{
[[nodiscard]] bool Finite3(const math::Float3& value) noexcept
{
    return
        std::isfinite(value.x) &&
        std::isfinite(value.y) &&
        std::isfinite(value.z);
}

[[nodiscard]] f32 SafePositive(
    const f32 value) noexcept
{
    return
        std::isfinite(value) && value > 0.0F
            ? value
            : 0.0F;
}

[[nodiscard]] math::Float3 SafeDirection(
    const math::Float3& value) noexcept
{
    if (!Finite3(value) ||
        math::LengthSquared(value) <= 1.0e-12F)
    {
        return {0.0F, -1.0F, 0.0F};
    }

    return math::Normalize(value);
}
} // namespace

ResolvedLocalLight ResolveLocalLight(
    const LocalLight& light,
    const LightingView& view) noexcept
{
    const f32 inner =
        std::clamp(
            light.innerConeRadians,
            0.0F,
            3.12413936F);
    const f32 outer =
        std::clamp(
            std::max(
                light.outerConeRadians,
                inner),
            0.0F,
            3.12413936F);

    return {
        .type = light.type,
        .positionCameraRelativeMeters =
            ToLightingCameraRelative(
                light.positionInFrameMeters,
                view),
        .direction =
            SafeDirection(light.direction),
        .colorLinear = {
            SafePositive(light.colorLinear.x),
            SafePositive(light.colorLinear.y),
            SafePositive(light.colorLinear.z)
        },
        .luminousFluxLumens =
            SafePositive(light.luminousFluxLumens),
        .rangeMeters =
            SafePositive(light.rangeMeters),
        .innerConeCosine =
            std::cos(inner),
        .outerConeCosine =
            std::cos(outer),
        .stableId =
            light.stableId
    };
}

TiledLightGrid BuildTiledLightGrid(
    const std::span<const LocalLight> lights,
    const LightingView& view,
    const u32 viewportWidth,
    const u32 viewportHeight,
    const TiledLightGridConfig& config)
{
    if (config.tileSizePixels == 0U ||
        config.maximumLightsPerTile == 0U)
    {
        throw std::invalid_argument(
            "Tiled light grid dimensions must be non-zero.");
    }

    TiledLightGrid result{
        .viewportWidth = viewportWidth,
        .viewportHeight = viewportHeight,
        .tileSizePixels = config.tileSizePixels,
        .tilesX =
            viewportWidth == 0U
                ? 0U
                : (viewportWidth +
                   config.tileSizePixels - 1U) /
                    config.tileSizePixels,
        .tilesY =
            viewportHeight == 0U
                ? 0U
                : (viewportHeight +
                   config.tileSizePixels - 1U) /
                    config.tileSizePixels
    };

    if (result.tilesX == 0U ||
        result.tilesY == 0U)
    {
        result.offsets = {0U};
        return result;
    }

    result.lights.reserve(lights.size());

    const math::Float3 forward =
        SafeDirection(view.forward);

    math::Float3 requestedUp =
        SafeDirection(view.up);

    math::Float3 right =
        math::Cross(
            forward,
            requestedUp);

    if (math::LengthSquared(right) <=
        1.0e-12F)
    {
        requestedUp =
            {0.0F, 1.0F, 0.0F};
        right =
            math::Cross(
                forward,
                requestedUp);
    }

    right = SafeDirection(right);

    const math::Float3 up =
        SafeDirection(
            math::Cross(
                right,
                forward));

    const f32 aspect =
        static_cast<f32>(viewportWidth) /
        static_cast<f32>(
            std::max(viewportHeight, 1U));

    const f32 tanHalf =
        std::max(
            std::tan(
                view.verticalFovRadians *
                0.5F),
            0.001F);

    const u32 tileCount =
        result.tilesX * result.tilesY;

    std::vector<std::vector<u32>>
        tileLists(tileCount);

    for (const LocalLight& source : lights)
    {
        const ResolvedLocalLight light =
            ResolveLocalLight(
                source,
                view);

        if (light.luminousFluxLumens <= 0.0F ||
            light.rangeMeters <= 0.0F)
        {
            continue;
        }

        const f32 z =
            math::Dot(
                light.positionCameraRelativeMeters,
                forward);

        if (z + light.rangeMeters <= 0.0F)
        {
            continue;
        }

        const u32 resolvedIndex =
            static_cast<u32>(
                result.lights.size());

        result.lights.push_back(light);

        // Project the light sphere conservatively. Lights that overlap/contain
        // the camera cover every tile rather than producing unstable NDC math.
        i32 minTileX = 0;
        i32 minTileY = 0;
        i32 maxTileX =
            static_cast<i32>(
                result.tilesX) - 1;
        i32 maxTileY =
            static_cast<i32>(
                result.tilesY) - 1;

        if (z > light.rangeMeters)
        {
            const f32 x =
                math::Dot(
                    light.positionCameraRelativeMeters,
                    right);
            const f32 y =
                math::Dot(
                    light.positionCameraRelativeMeters,
                    up);

            const f32 centerX =
                x /
                (z * tanHalf * aspect);
            const f32 centerY =
                y /
                (z * tanHalf);

            const f32 radiusX =
                light.rangeMeters /
                (z * tanHalf * aspect);
            const f32 radiusY =
                light.rangeMeters /
                (z * tanHalf);

            const auto toPixelX =
                [viewportWidth](const f32 ndc)
                {
                    return
                        (ndc * 0.5F + 0.5F) *
                        static_cast<f32>(
                            viewportWidth);
                };

            const auto toPixelY =
                [viewportHeight](const f32 ndc)
                {
                    return
                        (0.5F - ndc * 0.5F) *
                        static_cast<f32>(
                            viewportHeight);
                };

            const f32 minPx =
                toPixelX(
                    centerX - radiusX);
            const f32 maxPx =
                toPixelX(
                    centerX + radiusX);
            const f32 minPy =
                toPixelY(
                    centerY + radiusY);
            const f32 maxPy =
                toPixelY(
                    centerY - radiusY);

            minTileX =
                static_cast<i32>(
                    std::floor(
                        minPx /
                        config.tileSizePixels));
            maxTileX =
                static_cast<i32>(
                    std::floor(
                        maxPx /
                        config.tileSizePixels));
            minTileY =
                static_cast<i32>(
                    std::floor(
                        minPy /
                        config.tileSizePixels));
            maxTileY =
                static_cast<i32>(
                    std::floor(
                        maxPy /
                        config.tileSizePixels));

            minTileX =
                std::clamp(
                    minTileX,
                    0,
                    static_cast<i32>(
                        result.tilesX) - 1);
            maxTileX =
                std::clamp(
                    maxTileX,
                    0,
                    static_cast<i32>(
                        result.tilesX) - 1);
            minTileY =
                std::clamp(
                    minTileY,
                    0,
                    static_cast<i32>(
                        result.tilesY) - 1);
            maxTileY =
                std::clamp(
                    maxTileY,
                    0,
                    static_cast<i32>(
                        result.tilesY) - 1);

            if (maxTileX < minTileX ||
                maxTileY < minTileY)
            {
                continue;
            }
        }

        for (i32 tileY = minTileY;
             tileY <= maxTileY;
             ++tileY)
        {
            for (i32 tileX = minTileX;
                 tileX <= maxTileX;
                 ++tileX)
            {
                auto& tile =
                    tileLists[
                        static_cast<u32>(tileY) *
                            result.tilesX +
                        static_cast<u32>(tileX)];

                if (tile.size() >=
                    config.maximumLightsPerTile)
                {
                    ++result.droppedAssignments;
                    continue;
                }

                tile.push_back(
                    resolvedIndex);
            }
        }
    }

    result.offsets.resize(
        tileCount + 1U,
        0U);

    for (u32 tile = 0U;
         tile < tileCount;
         ++tile)
    {
        result.offsets[tile + 1U] =
            result.offsets[tile] +
            static_cast<u32>(
                tileLists[tile].size());

        result.lightIndices.insert(
            result.lightIndices.end(),
            tileLists[tile].begin(),
            tileLists[tile].end());
    }

    return result;
}
} // namespace orbit::lighting
