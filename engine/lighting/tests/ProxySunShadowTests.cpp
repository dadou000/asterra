#include <orbit/lighting/ProxySunShadow.hpp>

#include <bit>
#include <cmath>
#include <cstdlib>

namespace
{
using namespace orbit;
using namespace orbit::lighting;

[[nodiscard]] f32 Float(const u32 bits)
{
    return std::bit_cast<f32>(bits);
}

bool Near(const f32 a, const f32 b)
{
    return std::fabs(a - b) < 1.0e-5F;
}
} // namespace

int RunProxySurfaceTests();

int main()
{
    LightingView view;
    view.forward = {0.0F, 0.0F, -1.0F};
    view.up = {0.0F, 1.0F, 0.0F};
    view.verticalFovRadians = 1.0F;
    view.nearPlaneMeters = 0.25F;
    view.farPlaneMeters = 5000.0F;

    const auto constants =
        PackProxySunShadowConstants(
            view,
            1920U,
            1080U,
            17U,
            {0.0F, 0.6F, 0.8F},
            {10.0, -20.0, 30.0},
            {.maximumDistanceMeters = 1500.0F,
             .normalBiasMeters = 0.05F,
             .skyIrradiance = {0.1F, 0.2F, 0.4F},
             .skyRayCount = 200U,
             .skyMaximumDistanceMeters = 90.0F});

    // The compute shader reads these 32 dwords as one push-constant block
    // (uint3 + float, then seven float4); a reorder here must be mirrored there.
    if (constants.size() != 32U ||
        constants[0] != 1920U ||
        constants[1] != 1080U ||
        constants[2] != 17U ||
        !Near(Float(constants[3]), 1500.0F) ||
        !Near(Float(constants[6]), -1.0F) ||
        !Near(Float(constants[7]), 1920.0F / 1080.0F) ||
        !Near(Float(constants[9]), 1.0F) ||
        !Near(Float(constants[11]), std::tan(0.5F)) ||
        !Near(Float(constants[12]), 0.25F) ||
        !Near(Float(constants[13]), 5000.0F) ||
        !Near(Float(constants[16]), 10.0F) ||
        !Near(Float(constants[17]), -20.0F) ||
        !Near(Float(constants[18]), 30.0F) ||
        !Near(Float(constants[21]), 0.6F) ||
        !Near(Float(constants[22]), 0.8F) ||
        !Near(Float(constants[23]), 0.05F) ||
        !Near(Float(constants[14]), 64.0F) ||
        !Near(Float(constants[24]), 0.1F) ||
        !Near(Float(constants[25]), 0.2F) ||
        !Near(Float(constants[26]), 0.4F) ||
        !Near(Float(constants[27]), 90.0F) ||
        // no camera position: the local up falls back to +Y
        !Near(Float(constants[28]), 0.0F) ||
        !Near(Float(constants[29]), 1.0F) ||
        !Near(Float(constants[30]), 0.0F))
    {
        return EXIT_FAILURE;
    }

    // The local up is the camera's radial direction in the body frame.
    LightingView radial = view;
    radial.cameraPositionInFrameMeters = {0.0, 3.0e6, 4.0e6};

    const auto radialConstants =
        PackProxySunShadowConstants(
            radial,
            64U,
            64U,
            1U,
            {0.0F, 1.0F, 0.0F},
            {},
            {});

    if (!Near(Float(radialConstants[28]), 0.0F) ||
        !Near(Float(radialConstants[29]), 0.6F) ||
        !Near(Float(radialConstants[30]), 0.8F))
    {
        return EXIT_FAILURE;
    }

    // Degenerate settings are clamped rather than producing negative rays.
    const auto clamped =
        PackProxySunShadowConstants(
            view,
            64U,
            0U,
            0U,
            {0.0F, 1.0F, 0.0F},
            {},
            {.maximumDistanceMeters = -5.0F,
             .normalBiasMeters = -1.0F});

    if (!Near(Float(clamped[3]), 0.0F) ||
        !Near(Float(clamped[23]), 0.0F) ||
        !Near(Float(clamped[7]), 1.0F))
    {
        return EXIT_FAILURE;
    }

    return RunProxySurfaceTests();
}
