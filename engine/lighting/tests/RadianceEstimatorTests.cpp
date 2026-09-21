#include <orbit/lighting/RadianceEstimator.hpp>

int main()
{
    using namespace orbit;
    using namespace orbit::lighting;

    RadianceClipmapConfig config{
        .baseCellSizeMeters = 2.0,
        .levelScale = 4.0,
        .levelCount = 2U,
        .cellsPerAxis = 4U
    };

    LightingView view;
    view.frame = frames::FrameId{
        .high = 1U,
        .low = 2U};
    view.body = universe::BodyId{
        .high = 3U,
        .low = 4U};
    view.gpuOriginInFrameMeters = {};

    const auto key =
        RadianceCellForPoint(
            {0.0, 0.0, 0.0},
            config,
            0U,
            view);

    const DirectionalLight stellar{
        .directionToLight =
            {0.0F, 1.0F, 0.0F},
        .colorLinear =
            {1.0F, 0.9F, 0.8F},
        .irradianceScale = 1.0F
    };

    const auto noLocal =
        EstimateRadianceCell(
            key,
            config,
            view,
            stellar,
            {});

    if (noLocal.l0.x <= 0.0F ||
        noLocal.l1y.x <= 0.0F)
    {
        return 1;
    }

    const ResolvedLocalLight lamp{
        .type = LocalLightType::Point,
        .positionCameraRelativeMeters =
            {2.0F, 1.0F, 1.0F},
        .colorLinear =
            {1.0F, 0.2F, 0.1F},
        .luminousFluxLumens = 4000.0F,
        .rangeMeters = 20.0F
    };

    const auto withLocal =
        EstimateRadianceCell(
            key,
            config,
            view,
            stellar,
            std::span<const ResolvedLocalLight>(
                &lamp,
                1U));

    if (withLocal.l0.x <=
            noLocal.l0.x ||
        withLocal.l1x.x <=
            noLocal.l1x.x)
    {
        return 2;
    }

    return 0;
}
