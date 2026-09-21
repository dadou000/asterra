#include <orbit/lighting/EmissiveHierarchy.hpp>

#include <array>

int main()
{
    using namespace orbit;
    using namespace orbit::lighting;

    constexpr u32 width = 8U;
    constexpr u32 height = 4U;

    std::array<
        math::Float3,
        width * height>
        pixels{};

    for (u32 y = 0U; y < height; ++y)
    {
        for (u32 x = 0U; x < width; ++x)
        {
            pixels[
                y * width + x] =
                x < width / 2U
                    ? math::Float3{
                          8.0F, 0.2F, 0.1F}
                    : math::Float3{
                          0.1F, 0.3F, 9.0F};
        }
    }

    const frames::FrameId frame{
        .high = 1U,
        .low = 2U};
    const universe::BodyId body{
        .high = 3U,
        .low = 4U};

    const auto hierarchy =
        BuildEmissiveHierarchy({
            .frame = frame,
            .body = body,
            .stableId = 42U,
            .originInFrameMeters =
                {-2.0, -1.0, 0.0},
            .axisUInFrameMeters =
                {4.0, 0.0, 0.0},
            .axisVInFrameMeters =
                {0.0, 2.0, 0.0},
            .width = width,
            .height = height,
            .giRadiance = pixels
        },
        {
            .leafTileWidth = 1U,
            .leafTileHeight = 1U
        });

    if (hierarchy.nodes.empty() ||
        hierarchy.root >=
            hierarchy.nodes.size() ||
        hierarchy.nodes[
            hierarchy.root].
            radiantImportance <= 0.0)
    {
        return 1;
    }

    const auto& rootNode =
        hierarchy.nodes[
            hierarchy.root];

    if (rootNode.integratedRadianceArea.x <= 0.0F ||
        rootNode.integratedRadianceArea.z <= 0.0F ||
        rootNode.peakLuminance <= 0.0 ||
        rootNode.energyWeightedUv.x <= 0.0 ||
        rootNode.energyWeightedUv.x >= 1.0 ||
        rootNode.energyWeightedUv.y <= 0.0 ||
        rootNode.energyWeightedUv.y >= 1.0)
    {
        return 5;
    }

    LightingView view;
    view.frame = frame;
    view.body = body;
    view.forward =
        {0.0F, 0.0F, 1.0F};
    view.up =
        {0.0F, 1.0F, 0.0F};
    view.verticalFovRadians =
        1.0F;

    view.cameraPositionInFrameMeters =
        {0.0, 0.0, -3.0};

    const auto nearSelection =
        SelectEmissiveHierarchy(
            hierarchy,
            view,
            1920U,
            1080U,
            {
                .subdivisionProjectedPixels =
                    20.0F,
                .minimumRadiantImportance =
                    1.0e30,
                .maximumSelectedNodes =
                    256U
            });

    view.cameraPositionInFrameMeters =
        {0.0, 0.0, -300.0};

    const auto farSelection =
        SelectEmissiveHierarchy(
            hierarchy,
            view,
            1920U,
            1080U,
            {
                .subdivisionProjectedPixels =
                    20.0F,
                .minimumRadiantImportance =
                    1.0e30,
                .maximumSelectedNodes =
                    256U
            });

    if (nearSelection.size() <=
            farSelection.size() ||
        farSelection.size() != 1U)
    {
        return 2;
    }

    bool sawRed = false;
    bool sawBlue = false;

    for (const auto selected :
         nearSelection)
    {
        const auto& node =
            hierarchy.nodes[
                selected.nodeIndex];

        sawRed |=
            node.averageRadiance.x >
            node.averageRadiance.z;

        sawBlue |=
            node.averageRadiance.z >
            node.averageRadiance.x;
    }

    if (!sawRed || !sawBlue)
    {
        return 3;
    }

    // One hierarchy node collection represents the whole display: no Light
    // object is created per source texel.
    if (hierarchy.nodes.size() >=
        pixels.size() * 2U)
    {
        return 4;
    }

    return 0;
}
