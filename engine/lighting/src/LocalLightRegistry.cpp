#include <orbit/lighting/LocalLightRegistry.hpp>

#include <map>

#include <algorithm>
#include <array>
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

GpuLocalLight EncodeGpuLocalLight(
    const ResolvedLocalLight& light) noexcept
{
    return {
        .positionType = {
            light.positionCameraRelativeMeters.x,
            light.positionCameraRelativeMeters.y,
            light.positionCameraRelativeMeters.z,
            light.type == LocalLightType::Spot
                ? 1.0F
                : 0.0F
        },
        .directionRange = {
            light.direction.x,
            light.direction.y,
            light.direction.z,
            light.rangeMeters
        },
        .colorFlux = {
            light.colorLinear.x,
            light.colorLinear.y,
            light.colorLinear.z,
            light.luminousFluxLumens
        },
        .cone = {
            light.innerConeCosine,
            light.outerConeCosine,
            0.0F,
            0.0F
        }
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

    // Flat two-pass layout (count, then fill) instead of one heap vector per
    // tile: a 4K viewport has ~11k tiles and a wide light covers all of them.
    std::vector<std::array<i32, 5>>
        lightTileRects;
    lightTileRects.reserve(lights.size());
    // Contribution bound per rect: luminance-weighted flux over the squared
    // distance to the light's nearest point to the camera (>= 1 m).
    std::vector<f32> lightScores;
    lightScores.reserve(lights.size());
    std::vector<u32>
        tileCounts(tileCount, 0U);
    std::vector<u32>
        coverage(tileCount, 0U);

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

        lightTileRects.push_back(
            {minTileX, minTileY, maxTileX, maxTileY,
             static_cast<i32>(resolvedIndex)});

        const f32 luminance =
            0.2126F * light.colorLinear.x +
            0.7152F * light.colorLinear.y +
            0.0722F * light.colorLinear.z;
        const f32 nearest =
            std::max(
                std::sqrt(math::LengthSquared(
                    light.positionCameraRelativeMeters)) -
                    light.rangeMeters,
                1.0F);
        lightScores.push_back(
            light.luminousFluxLumens * luminance /
            (nearest * nearest));

        for (i32 tileY = minTileY;
             tileY <= maxTileY;
             ++tileY)
        {
            for (i32 tileX = minTileX;
                 tileX <= maxTileX;
                 ++tileX)
            {
                ++coverage[
                    static_cast<std::size_t>(tileY) * result.tilesX +
                    static_cast<std::size_t>(tileX)];
            }
        }
    }

    // Strongest first; stable so equal scores keep submission order. Both the
    // count and fill passes walk this order, so every tile keeps the same
    // strongest lights.
    std::vector<u32> order(lightTileRects.size());
    for (u32 i = 0U; i < order.size(); ++i)
    {
        order[i] = i;
    }
    if (config.aggregateOverflow)
    {
        std::stable_sort(
            order.begin(),
            order.end(),
            [&lightScores](const u32 a, const u32 b)
            {
                return lightScores[a] > lightScores[b];
            });
    }

    // Tiles that overflow reserve their last slot for the proxy.
    std::vector<u32> tileCapacity(tileCount);
    for (u32 tile = 0U; tile < tileCount; ++tile)
    {
        tileCapacity[tile] =
            config.aggregateOverflow &&
                    coverage[tile] > config.maximumLightsPerTile
                ? config.maximumLightsPerTile - 1U
                : config.maximumLightsPerTile;
    }

    for (const u32 rectIndex : order)
    {
        const auto& rect = lightTileRects[rectIndex];
        for (i32 tileY = rect[1]; tileY <= rect[3]; ++tileY)
        {
            for (i32 tileX = rect[0]; tileX <= rect[2]; ++tileX)
            {
                const u32 tile =
                    static_cast<u32>(tileY) * result.tilesX +
                    static_cast<u32>(tileX);
                u32& count = tileCounts[tile];
                if (count >= tileCapacity[tile])
                {
                    ++result.droppedAssignments;
                    continue;
                }
                ++count;
            }
        }
    }

    if (config.aggregateOverflow)
    {
        for (u32 tile = 0U; tile < tileCount; ++tile)
        {
            if (coverage[tile] > config.maximumLightsPerTile)
            {
                ++tileCounts[tile];   // the proxy slot
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
            tileCounts[tile];
    }

    result.lightIndices.resize(
        result.offsets[tileCount]);

    // Refill in the same order; lights that no longer fit are collected per
    // overflowing tile and replaced by one proxy.
    std::fill(
        tileCounts.begin(),
        tileCounts.end(),
        0U);

    std::map<u32, std::vector<u32>> overflow;
    for (const u32 rectIndex : order)
    {
        const auto& rect = lightTileRects[rectIndex];
        for (i32 tileY = rect[1]; tileY <= rect[3]; ++tileY)
        {
            for (i32 tileX = rect[0]; tileX <= rect[2]; ++tileX)
            {
                const u32 tile =
                    static_cast<u32>(tileY) * result.tilesX +
                    static_cast<u32>(tileX);
                u32& count = tileCounts[tile];
                if (count >= tileCapacity[tile])
                {
                    if (config.aggregateOverflow)
                    {
                        overflow[tile].push_back(
                            static_cast<u32>(rect[4]));
                    }
                    continue;
                }
                result.lightIndices[
                    result.offsets[tile] + count] =
                    static_cast<u32>(rect[4]);
                ++count;
            }
        }
    }

    // One proxy per distinct dropped set: neighbouring tiles overwhelmingly
    // drop the same lights, so they share it.
    std::map<std::vector<u32>, u32> proxyOfSet;
    for (auto& [tile, dropped] : overflow)
    {
        std::sort(dropped.begin(), dropped.end());
        auto found = proxyOfSet.find(dropped);
        if (found == proxyOfSet.end())
        {
            double flux = 0.0;
            double px = 0.0, py = 0.0, pz = 0.0;
            double cr = 0.0, cg = 0.0, cb = 0.0;
            u64 id = 1469598103934665603ULL;   // FNV-1a over member ids
            for (const u32 index : dropped)
            {
                const ResolvedLocalLight& member = result.lights[index];
                const double f = member.luminousFluxLumens;
                flux += f;
                px += f * member.positionCameraRelativeMeters.x;
                py += f * member.positionCameraRelativeMeters.y;
                pz += f * member.positionCameraRelativeMeters.z;
                cr += f * member.colorLinear.x;
                cg += f * member.colorLinear.y;
                cb += f * member.colorLinear.z;
                id = (id ^ member.stableId) * 1099511628211ULL;
            }
            ResolvedLocalLight proxy;
            proxy.type = LocalLightType::Point;
            proxy.luminousFluxLumens = static_cast<f32>(flux);
            proxy.positionCameraRelativeMeters = {
                static_cast<f32>(px / flux),
                static_cast<f32>(py / flux),
                static_cast<f32>(pz / flux)};
            proxy.colorLinear = {
                static_cast<f32>(cr / flux),
                static_cast<f32>(cg / flux),
                static_cast<f32>(cb / flux)};
            f32 reach = 0.0F;
            for (const u32 index : dropped)
            {
                const ResolvedLocalLight& member = result.lights[index];
                const math::Float3 d{
                    member.positionCameraRelativeMeters.x -
                        proxy.positionCameraRelativeMeters.x,
                    member.positionCameraRelativeMeters.y -
                        proxy.positionCameraRelativeMeters.y,
                    member.positionCameraRelativeMeters.z -
                        proxy.positionCameraRelativeMeters.z};
                reach = std::max(
                    reach,
                    std::sqrt(math::LengthSquared(d)) +
                        member.rangeMeters);
            }
            proxy.rangeMeters = reach;
            proxy.stableId = id | kProxyLightIdBit;
            found = proxyOfSet.emplace(
                dropped,
                static_cast<u32>(result.lights.size())).first;
            result.lights.push_back(proxy);
            ++result.proxyLights;
        }
        result.lightIndices[
            result.offsets[tile] + tileCounts[tile]] = found->second;
        ++tileCounts[tile];
    }

    return result;
}
} // namespace orbit::lighting
