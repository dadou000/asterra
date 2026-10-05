#include <orbit/lighting/ProxySurface.hpp>
#include <orbit/lighting/SurfaceBuffer.hpp>

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

// Packing is checked by ProxySunShadowTests' main through this entry point so
// both suites share one test executable.
int RunProxySurfaceTests()
{
    LightingView view;
    view.forward = {0.0F, 0.0F, 1.0F};
    view.up = {0.0F, 1.0F, 0.0F};
    view.verticalFovRadians = 1.0F;
    view.nearPlaneMeters = 0.5F;
    view.farPlaneMeters = 100.5F;

    const auto constants =
        PackProxySurfaceConstants(
            view,
            1600U,
            900U,
            {1.0, -2.0, 3.0},
            {.defaultAlbedo = {0.2F, 0.4F, 2.0F},
             .roughness = 0.7F});

    if (constants.size() != 24U)
    {
        return EXIT_FAILURE;
    }

    // Right-handed basis, as the lighting passes reconstruct positions:
    // right = forward x up. forward (0,0,1) x up (0,1,0) = (-1,0,0).
    const f32 yScale = 1.0F / std::tan(0.5F);
    const f32 xScale = yScale / (1600.0F / 900.0F);

    if (!Near(Float(constants[0]), -1.0F) ||
        !Near(Float(constants[3]), xScale) ||
        !Near(Float(constants[5]), 1.0F) ||
        !Near(Float(constants[7]), yScale) ||
        !Near(Float(constants[10]), 1.0F))
    {
        return EXIT_FAILURE;
    }

    // Reverse-Z depth terms match math::PerspectiveReverseZLH: near maps to 1
    // and far to 0 through (z * scale + bias) / z.
    const f32 scale = Float(constants[11]);
    const f32 bias = Float(constants[15]);
    if (!Near((scale * 0.5F + bias) / 0.5F, 1.0F) ||
        std::fabs((scale * 100.5F + bias) / 100.5F) > 1.0e-4F)
    {
        return EXIT_FAILURE;
    }

    if (!Near(Float(constants[12]), 1.0F) ||
        !Near(Float(constants[13]), -2.0F) ||
        !Near(Float(constants[14]), 3.0F))
    {
        return EXIT_FAILURE;
    }

    // Albedo is clamped, roughness passes through, metadata is the shared
    // RigidGeometry / LocalMesh encoding the deferred passes decode.
    const auto decoded =
        DecodeSurfaceMetadata(Float(constants[20]));
    if (!Near(Float(constants[16]), 0.2F) ||
        !Near(Float(constants[18]), 1.0F) ||
        !Near(Float(constants[19]), 0.7F) ||
        decoded.surfaceClass != SurfaceClass::RigidGeometry ||
        decoded.representation !=
            SurfaceRepresentation::LocalMesh)
    {
        return EXIT_FAILURE;
    }

    // The GPU scenes are only exact near the origin they were built at. A
    // camera that has drifted past the threshold while near the proxies needs a
    // refreshed origin; far from the proxies, or within the threshold, it does
    // not (a planet-scale view must not rebuild every frame).
    const math::Double3 origin{6'371'000.0, 0.0, 0.0};
    const math::Double3 proxies{6'371'050.0, 20.0, 0.0};

    if (ProxyGpuOriginIsStale(origin, origin, proxies, 15.0) ||
        ProxyGpuOriginIsStale(
            origin + math::Double3{1'000.0, 0.0, 0.0},
            origin,
            proxies,
            15.0) ||
        !ProxyGpuOriginIsStale(
            origin + math::Double3{2'500.0, 0.0, 0.0},
            origin,
            proxies,
            15.0) ||
        ProxyGpuOriginIsStale(
            origin + math::Double3{9.0e8, 0.0, 0.0},
            origin,
            proxies,
            15.0))
    {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
