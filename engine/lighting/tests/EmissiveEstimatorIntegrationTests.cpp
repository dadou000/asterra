#include <orbit/lighting/EmissiveSampling.hpp>
#include <orbit/lighting/RadianceEstimator.hpp>

#include <array>

int main()
{
    using namespace orbit;
    using namespace orbit::lighting;

    constexpr u32 width = 4U;
    constexpr u32 height = 4U;

    std::array<
        math::Float3,
        width * height>
        pixels{};

    for (auto& pixel : pixels)
    {
        pixel = {4.0F, 1.0F, 0.2F};
    }

    const frames::FrameId frame{
        .high = 1U,
        .low = 2U};
    const universe::BodyId body{
        .high = 3U,
        .low = 4U};

    const auto hierarchy =
        BuildEmissiveHierarchy(
            {
                .frame = frame,
                .body = body,
                .stableId = 9U,
                .originInFrameMeters =
                    {-1.0, -1.0, 4.0},
                .axisUInFrameMeters =
                    {2.0, 0.0, 0.0},
                // Winding makes normal face toward -Z, i.e. toward cell/camera.
                .axisVInFrameMeters =
                    {0.0, -2.0, 0.0},
                .width = width,
                .height = height,
                .giRadiance = pixels
            },
            {
                .leafTileWidth = 2U,
                .leafTileHeight = 2U
            });

    LightingView view;
    view.frame = frame;
    view.body = body;
    view.cameraPositionInFrameMeters =
        {0.0, 0.0, 0.0};
    view.gpuOriginInFrameMeters =
        {0.0, 0.0, 0.0};
    view.verticalFovRadians = 1.0F;
    view.farPlaneMeters = 1000.0F;

    const auto emitters =
        BuildEmissiveSampleSet(
            hierarchy,
            view,
            1920U,
            1080U,
            {
                .subdivisionProjectedPixels =
                    1.0F,
                .smallEmitterPeakLuminance =
                    1000.0,
                .maximumSamples = 64U
            });

    if (emitters.emitters.empty() ||
        !emitters.EnergyPartitionValid())
    {
        return 1;
    }

    const RadianceClipmapConfig config{
        .baseCellSizeMeters = 1.0,
        .levelScale = 2.0,
        .levelCount = 1U,
        .cellsPerAxis = 4U
    };

    const auto key =
        RadianceCellForPoint(
            {0.0, 0.0, 0.0},
            config,
            0U,
            view);

    const DirectionalLight noSun{
        .directionToLight =
            {0.0F, 1.0F, 0.0F},
        .colorLinear = {},
        .irradianceScale = 0.0F
    };

    const auto baseline =
        EstimateRadianceCell(
            key,
            config,
            view,
            noSun,
            {},
            nullptr,
            {
                .diffuseTransportScale = 1.0F,
                .ambientIrradianceScale = 0.0F
            });

    const auto lit =
        EstimateRadianceCell(
            key,
            config,
            view,
            noSun,
            {},
            nullptr,
            {
                .diffuseTransportScale = 1.0F,
                .ambientIrradianceScale = 0.0F
            },
            {},
            emitters.emitters);

    if (lit.l0.x <= baseline.l0.x ||
        lit.l1z.x <= baseline.l1z.x)
    {
        return 2;
    }

    // Flip the surface normal by reversing V winding: the one-sided emitter
    // should no longer illuminate the cell behind it.
    const auto backHierarchy =
        BuildEmissiveHierarchy(
            {
                .frame = frame,
                .body = body,
                .stableId = 10U,
                .originInFrameMeters =
                    {-1.0, -1.0, 4.0},
                .axisUInFrameMeters =
                    {2.0, 0.0, 0.0},
                .axisVInFrameMeters =
                    {0.0, 2.0, 0.0},
                .width = width,
                .height = height,
                .giRadiance = pixels
            });

    const auto backEmitters =
        BuildEmissiveSampleSet(
            backHierarchy,
            view,
            1920U,
            1080U);

    const auto backLit =
        EstimateRadianceCell(
            key,
            config,
            view,
            noSun,
            {},
            nullptr,
            {
                .diffuseTransportScale = 1.0F,
                .ambientIrradianceScale = 0.0F
            },
            {},
            backEmitters.emitters);

    if (backLit.l0.x != 0.0F)
    {
        return 3;
    }

    return 0;
}
