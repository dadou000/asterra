#include <orbit/lighting/RadianceEstimator.hpp>

namespace
{
class AlwaysHitProvider final
    : public orbit::lighting::VisibilityProvider
{
public:
    AlwaysHitProvider()
        : desc_({
            .providerId = 99U,
            .name = "Always Hit",
            .kind =
                orbit::lighting::
                    VisibilityBackendKind::SoftwareProxy,
            .capabilities =
                orbit::lighting::
                    VisibilityCapability::Offscreen,
            .nominalErrorMeters = 0.0F,
            .priority = 1
        })
    {
    }

    [[nodiscard]] const orbit::lighting::
        VisibilityProviderDesc&
    Description() const noexcept override
    {
        return desc_;
    }

    [[nodiscard]] bool SupportsPurpose(
        orbit::lighting::VisibilityPurpose) const noexcept override
    {
        return true;
    }

    [[nodiscard]] orbit::lighting::VisibilityResult Trace(
        const orbit::lighting::VisibilityQuery&) override
    {
        return {
            .resolution =
                orbit::lighting::
                    VisibilityResolution::Hit,
            .confidence = 1.0F,
            .terminal = true
        };
    }

private:
    orbit::lighting::VisibilityProviderDesc desc_;
};
} // namespace

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
            {},
            nullptr);

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
                1U),
            nullptr);

    if (withLocal.l0.x <=
            noLocal.l0.x ||
        withLocal.l1x.x <=
            noLocal.l1x.x)
    {
        return 2;
    }

    const EmissiveVolumeSource volume{
        .centerInFrameMeters =
            {5.0, 0.0, 0.0},
        .radiusMeters = 2.0F,
        .emissionLinear =
            {0.1F, 3.0F, 0.4F},
        .intensityScale = 1.0F,
        .influenceRangeMeters = 30.0F,
        .stableId = 77U
    };

    const auto withVolume =
        EstimateRadianceCell(
            key,
            config,
            view,
            DirectionalLight{},
            {},
            nullptr,
            {
                .diffuseTransportScale = 1.0F,
                .ambientIrradianceScale = 0.0F
            },
            std::span<const EmissiveVolumeSource>(
                &volume,
                1U));

    if (withVolume.l0.y <= 0.0F ||
        withVolume.l1x.y <= 0.0F ||
        withVolume.l0.y <=
            withVolume.l0.x)
    {
        return 3;
    }

    AlwaysHitProvider blocker;

    const auto blocked =
        EstimateRadianceCell(
            key,
            config,
            view,
            stellar,
            std::span<const ResolvedLocalLight>(
                &lamp,
                1U),
            &blocker);

    // Ambient remains, but stellar/local directional energy is suppressed.
    if (blocked.l1x.x != 0.0F ||
        blocked.l1y.x != 0.0F ||
        blocked.l1z.x != 0.0F ||
        blocked.l0.x >= noLocal.l0.x)
    {
        return 3;
    }

    return 0;
}
