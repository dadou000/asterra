#include <orbit/lighting/EmissiveHierarchy.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace orbit::lighting
{
namespace
{
[[nodiscard]] f64 Luminance(
    const math::Float3 value) noexcept
{
    return
        0.2126 *
            static_cast<f64>(
                std::max(value.x, 0.0F)) +
        0.7152 *
            static_cast<f64>(
                std::max(value.y, 0.0F)) +
        0.0722 *
            static_cast<f64>(
                std::max(value.z, 0.0F));
}

struct Builder
{
    const EmissiveSurfaceGrid* surface{nullptr};
    EmissiveHierarchy result;
    f64 texelArea{0.0};
    EmissiveHierarchyBuildConfig config{};

    [[nodiscard]] u32 Build(
        const u32 minX,
        const u32 minY,
        const u32 maxX,
        const u32 maxY,
        const u32 level)
    {
        const u32 index =
            static_cast<u32>(
                result.nodes.size());

        result.nodes.emplace_back();

        auto& placeholder =
            result.nodes[index];

        placeholder.texelMinX = minX;
        placeholder.texelMinY = minY;
        placeholder.texelMaxX = maxX;
        placeholder.texelMaxY = maxY;
        placeholder.level = level;

        math::Double3 center{};
        math::Float3 sum{};
        math::Float3 peak{};
        math::Float3 integrated{};
        f64 importance = 0.0;
        f64 peakLuminance = 0.0;
        math::Double2 weightedUv{};

        const u64 texelCount =
            static_cast<u64>(
                maxX - minX) *
            static_cast<u64>(
                maxY - minY);

        for (u32 y = minY; y < maxY; ++y)
        {
            for (u32 x = minX; x < maxX; ++x)
            {
                const auto radiance =
                    surface->giRadiance[
                        static_cast<std::size_t>(y) *
                            surface->width +
                        x];

                sum.x += std::max(radiance.x, 0.0F);
                sum.y += std::max(radiance.y, 0.0F);
                sum.z += std::max(radiance.z, 0.0F);

                peak.x =
                    std::max(
                        peak.x,
                        std::max(
                            radiance.x,
                            0.0F));
                peak.y =
                    std::max(
                        peak.y,
                        std::max(
                            radiance.y,
                            0.0F));
                peak.z =
                    std::max(
                        peak.z,
                        std::max(
                            radiance.z,
                            0.0F));

                const f64 luminance =
                    Luminance(radiance);

                const f64 weight =
                    luminance *
                    texelArea;

                importance +=
                    weight;

                peakLuminance =
                    std::max(
                        peakLuminance,
                        luminance);

                integrated.x +=
                    std::max(radiance.x, 0.0F) *
                    static_cast<f32>(texelArea);
                integrated.y +=
                    std::max(radiance.y, 0.0F) *
                    static_cast<f32>(texelArea);
                integrated.z +=
                    std::max(radiance.z, 0.0F) *
                    static_cast<f32>(texelArea);

                weightedUv.x +=
                    ((static_cast<f64>(x) + 0.5) /
                     static_cast<f64>(surface->width)) *
                    weight;
                weightedUv.y +=
                    ((static_cast<f64>(y) + 0.5) /
                     static_cast<f64>(surface->height)) *
                    weight;
            }
        }

        const f64 inverseCount =
            texelCount > 0U
                ? 1.0 /
                    static_cast<f64>(
                        texelCount)
                : 0.0;

        const f64 centerU =
            (static_cast<f64>(minX + maxX) *
             0.5) /
            static_cast<f64>(
                surface->width);

        const f64 centerV =
            (static_cast<f64>(minY + maxY) *
             0.5) /
            static_cast<f64>(
                surface->height);

        center =
            surface->originInFrameMeters +
            surface->axisUInFrameMeters *
                centerU +
            surface->axisVInFrameMeters *
                centerV;

        // result.nodes can reallocate while children are appended, so write
        // parent fields through index after all recursive calls.
        result.nodes[index].centerInFrameMeters =
            center;
        result.nodes[index].averageRadiance = {
            static_cast<f32>(
                static_cast<f64>(sum.x) *
                inverseCount),
            static_cast<f32>(
                static_cast<f64>(sum.y) *
                inverseCount),
            static_cast<f32>(
                static_cast<f64>(sum.z) *
                inverseCount)
        };
        result.nodes[index].peakRadiance =
            peak;
        result.nodes[index].integratedRadianceArea =
            integrated;
        result.nodes[index].peakLuminance =
            peakLuminance;
        result.nodes[index].energyWeightedUv =
            importance > 1.0e-20
                ? math::Double2{
                      weightedUv.x / importance,
                      weightedUv.y / importance}
                : math::Double2{
                      centerU,
                      centerV};
        result.nodes[index].areaMetersSquared =
            texelArea *
            static_cast<f64>(
                texelCount);
        result.nodes[index].radiantImportance =
            importance;

        const u32 width =
            maxX - minX;
        const u32 height =
            maxY - minY;

        if (width <=
                std::max(
                    config.leafTileWidth,
                    1U) &&
            height <=
                std::max(
                    config.leafTileHeight,
                    1U))
        {
            return index;
        }

        const u32 splitX =
            width > 1U
                ? minX + width / 2U
                : maxX;

        const u32 splitY =
            height > 1U
                ? minY + height / 2U
                : maxY;

        std::array<
            std::array<u32, 4>,
            4> regions{};
        u32 regionCount = 0U;

        const auto addRegion =
            [&](const u32 x0,
                const u32 y0,
                const u32 x1,
                const u32 y1)
            {
                if (x1 > x0 &&
                    y1 > y0)
                {
                    regions[regionCount++] = {
                        x0, y0, x1, y1
                    };
                }
            };

        addRegion(
            minX,
            minY,
            splitX,
            splitY);
        addRegion(
            splitX,
            minY,
            maxX,
            splitY);
        addRegion(
            minX,
            splitY,
            splitX,
            maxY);
        addRegion(
            splitX,
            splitY,
            maxX,
            maxY);

        result.nodes[index].childCount =
            regionCount;

        for (u32 region = 0U;
             region < regionCount;
             ++region)
        {
            const auto r =
                regions[region];

            result.nodes[index].
                children[region] =
                    Build(
                        r[0],
                        r[1],
                        r[2],
                        r[3],
                        level + 1U);
        }

        return index;
    }
};

[[nodiscard]] f32 ProjectedPixels(
    const EmissiveHierarchyNode& node,
    const EmissiveHierarchy& hierarchy,
    const LightingView& view,
    const u32 viewportWidth,
    const u32 viewportHeight) noexcept
{
    if (viewportWidth == 0U ||
        viewportHeight == 0U)
    {
        return 0.0F;
    }

    const auto relativeD =
        node.centerInFrameMeters -
        view.cameraPositionInFrameMeters;

    const f64 distanceSquared =
        math::LengthSquared(
            relativeD);

    if (!std::isfinite(distanceSquared) ||
        distanceSquared <= 1.0e-12)
    {
        return
            static_cast<f32>(
                viewportWidth *
                viewportHeight);
    }

    const f64 distance =
        std::sqrt(
            distanceSquared);

    const auto directionD =
        relativeD /
        distance;

    const f64 facing =
        std::abs(
            math::Dot(
                hierarchy.surfaceNormal,
                directionD));

    const f64 projectedArea =
        node.areaMetersSquared *
        facing /
        distanceSquared;

    const f64 focalPixels =
        static_cast<f64>(
            viewportHeight) /
        (2.0 *
         std::tan(
             static_cast<f64>(
                 view.verticalFovRadians) *
             0.5));

    const f64 pixelArea =
        projectedArea *
        focalPixels *
        focalPixels;

    if (!std::isfinite(pixelArea) ||
        pixelArea <= 0.0)
    {
        return 0.0F;
    }

    return static_cast<f32>(
        std::sqrt(pixelArea));
}
} // namespace

EmissiveHierarchy BuildEmissiveHierarchy(
    const EmissiveSurfaceGrid& surface,
    const EmissiveHierarchyBuildConfig& config)
{
    if (config.leafTileWidth == 0U ||
        config.leafTileHeight == 0U)
    {
        throw std::invalid_argument(
            "Emissive hierarchy leaf tile dimensions must be non-zero.");
    }

    if (!surface.frame ||
        !surface.body ||
        surface.stableId == 0U ||
        surface.width == 0U ||
        surface.height == 0U ||
        surface.giRadiance.size() !=
            static_cast<std::size_t>(
                surface.width) *
            surface.height)
    {
        throw std::invalid_argument(
            "Emissive hierarchy requires valid authority, dimensions and radiance samples.");
    }

    const auto cross =
        math::Cross(
            surface.axisUInFrameMeters,
            surface.axisVInFrameMeters);

    const f64 area =
        math::Length(cross);

    if (!std::isfinite(area) ||
        area <= 1.0e-12)
    {
        throw std::invalid_argument(
            "Emissive surface axes must span non-zero physical area.");
    }

    Builder builder;
    builder.surface = &surface;
    builder.result.frame =
        surface.frame;
    builder.result.body =
        surface.body;
    builder.result.stableId =
        surface.stableId;
    builder.result.surfaceNormal =
        math::Normalize(cross);
    builder.result.sourceWidth =
        surface.width;
    builder.result.sourceHeight =
        surface.height;
    builder.config = config;
    builder.texelArea =
        area /
        (static_cast<f64>(
             surface.width) *
         static_cast<f64>(
             surface.height));

    builder.result.root =
        builder.Build(
            0U,
            0U,
            surface.width,
            surface.height,
            0U);

    return builder.result;
}

std::vector<EmissiveSelectedNode>
SelectEmissiveHierarchy(
    const EmissiveHierarchy& hierarchy,
    const LightingView& view,
    const u32 viewportWidth,
    const u32 viewportHeight,
    const EmissiveHierarchySelectionConfig& config)
{
    std::vector<EmissiveSelectedNode> selected;

    if (hierarchy.root >=
            hierarchy.nodes.size() ||
        view.frame != hierarchy.frame ||
        view.body != hierarchy.body ||
        config.maximumSelectedNodes == 0U)
    {
        return selected;
    }

    std::vector<u32> stack{
        hierarchy.root};

    while (!stack.empty() &&
           selected.size() <
               config.maximumSelectedNodes)
    {
        const u32 nodeIndex =
            stack.back();
        stack.pop_back();

        const auto& node =
            hierarchy.nodes[nodeIndex];

        const f32 projected =
            ProjectedPixels(
                node,
                hierarchy,
                view,
                viewportWidth,
                viewportHeight);

        const f64 weightedImportance =
            node.radiantImportance *
            static_cast<f64>(
                std::max(
                    projected,
                    0.0F));

        const bool refineForEnergy =
            config.minimumRadiantImportance > 0.0 &&
            weightedImportance >=
                config.minimumRadiantImportance;

        const bool refine =
            !node.IsLeaf() &&
            (projected >=
                 config.
                     subdivisionProjectedPixels ||
             refineForEnergy);

        if (refine &&
            selected.size() +
                    stack.size() +
                    node.childCount <=
                config.maximumSelectedNodes)
        {
            for (u32 child = 0U;
                 child < node.childCount;
                 ++child)
            {
                stack.push_back(
                    node.children[
                        node.childCount -
                        1U -
                        child]);
            }

            continue;
        }

        selected.push_back({
            .nodeIndex = nodeIndex,
            .projectedPixels =
                projected,
            .weightedImportance =
                weightedImportance
        });
    }

    return selected;
}
} // namespace orbit::lighting
