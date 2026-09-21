#include <orbit/lighting/RuntimeEmissiveSurface.hpp>

#include <array>
#include <cmath>

int main()
{
    using namespace orbit;
    using namespace orbit::lighting;

    const frames::FrameId frame{
        .high = 1U,
        .low = 2U};
    const universe::BodyId body{
        .high = 3U,
        .low = 4U};

    const RuntimeEmissiveSurfaceGeometry geometry{
        .frame = frame,
        .body = body,
        .stableId = 77U,
        .originInFrameMeters =
            {-1.0, -0.5, 10.0},
        .axisUInFrameMeters =
            {2.0, 0.0, 0.0},
        .axisVInFrameMeters =
            {0.0, 1.0, 0.0}
    };

    const EvaluatedMaterialEmission emission{
        .visibleRadiance =
            {10.0F, 20.0F, 30.0F},
        .giRadiance =
            {4.0F, 8.0F, 12.0F}
    };

    const std::array<std::byte, 8> pixels{
        std::byte{0xff},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0xff},

        std::byte{0x00},
        std::byte{0x00},
        std::byte{0xff},
        std::byte{0xff}
    };

    const content::RuntimeTexture texture{
        .width = 2U,
        .height = 1U,
        .format =
            content::RuntimeTextureFormat::
                Rgba8Unorm,
        .pixels = {
            pixels.begin(),
            pixels.end()
        }
    };

    const auto surface =
        BuildRuntimeEmissiveSurface(
            geometry,
            emission,
            &texture,
            EmissiveTextureTransfer::Linear);

    if (surface.width != 2U ||
        surface.height != 1U ||
        surface.giRadiance.size() != 2U)
    {
        return 1;
    }

    if (std::abs(
            surface.giRadiance[0].x -
            4.0F) >
            1.0e-6F ||
        surface.giRadiance[0].y != 0.0F ||
        surface.giRadiance[0].z != 0.0F)
    {
        return 2;
    }

    if (surface.giRadiance[1].x != 0.0F ||
        surface.giRadiance[1].y != 0.0F ||
        std::abs(
            surface.giRadiance[1].z -
            12.0F) >
            1.0e-6F)
    {
        return 3;
    }

    const auto hierarchy =
        BuildEmissiveHierarchy(
            surface.Grid(),
            {
                .leafTileWidth = 1U,
                .leafTileHeight = 1U
            });

    if (hierarchy.nodes.empty() ||
        hierarchy.root >= hierarchy.nodes.size())
    {
        return 4;
    }

    LightingView view;
    view.frame = frame;
    view.body = body;
    view.gpuOriginInFrameMeters =
        {0.0, 0.0, 5.0};

    const auto gpu =
        EncodeGpuEmissiveHierarchy(
            hierarchy,
            view);

    if (gpu.size() !=
            hierarchy.nodes.size() ||
        sizeof(GpuEmissiveHierarchyNode) !=
            128U)
    {
        return 5;
    }

    const auto& root =
        gpu[hierarchy.root];

    if (root.metadata[0] == 0U ||
        root.metadata[1] != 2U ||
        root.metadata[2] != 1U ||
        std::abs(
            root.centerArea.z -
            5.0F) >
            1.0e-5F)
    {
        return 6;
    }

    const auto uniform =
        BuildRuntimeEmissiveSurface(
            geometry,
            emission,
            nullptr,
            EmissiveTextureTransfer::Linear);

    if (uniform.width != 1U ||
        uniform.height != 1U ||
        uniform.giRadiance.front().x !=
            emission.giRadiance.x)
    {
        return 7;
    }

    return 0;
}
