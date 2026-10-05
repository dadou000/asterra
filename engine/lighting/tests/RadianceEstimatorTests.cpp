#include <orbit/lighting/RadianceEstimator.hpp>

#include <cmath>

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

    const auto withSky =
        EstimateRadianceCell(
            key,
            config,
            view,
            DirectionalLight{
                .irradianceScale = 0.0F
            },
            {},
            nullptr,
            {
                .diffuseTransportScale = 1.0F,
                .ambientIrradianceScale = 0.0F,
                .skyIrradianceLinear = {
                    0.1F, 0.2F, 0.5F
                }
            });

    if (withSky.l0.z <= withSky.l0.y ||
        withSky.l0.y <= withSky.l0.x)
    {
        return 3;
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
        return 4;
    }

    AlwaysHitProvider skyBlocker;
    VisibilityRegistry skyBlockerRegistry;
    skyBlockerRegistry.Register(skyBlocker);

    const auto blockedSky =
        EstimateRadianceCell(
            key,
            config,
            view,
            DirectionalLight{
                .irradianceScale = 0.0F
            },
            {},
            &skyBlockerRegistry,
            {
                .diffuseTransportScale = 1.0F,
                .ambientIrradianceScale = 0.0F,
                .skyIrradianceLinear = {
                    0.1F, 0.2F, 0.5F
                }
            });

    if (blockedSky.l0.x != 0.0F ||
        blockedSky.l0.y != 0.0F ||
        blockedSky.l0.z != 0.0F)
    {
        return 5;
    }

    // Sky-only channel: full strength (no one-bounce transport), kept out of
    // the L1, pointing at the open sky.
    const RadianceEstimateSettings skySettings{
        .diffuseTransportScale = 0.18F,
        .ambientIrradianceScale = 0.0F,
        .skyIrradianceLinear = {0.1F, 0.2F, 0.5F}
    };

    const auto channelSky =
        EstimateRadianceCellWithSky(
            key,
            config,
            view,
            DirectionalLight{
                .irradianceScale = 0.0F
            },
            {},
            nullptr,
            skySettings);

    const f32 gradientLength =
        math::Length(channelSky.sky.gradient);

    if (channelSky.indirect.l0.x != 0.0F ||
        channelSky.indirect.l0.y != 0.0F ||
        channelSky.indirect.l0.z != 0.0F ||
        std::fabs(channelSky.sky.l0.x - 0.05F) > 1.0e-6F ||
        std::fabs(channelSky.sky.l0.y - 0.10F) > 1.0e-6F ||
        std::fabs(channelSky.sky.l0.z - 0.25F) > 1.0e-6F ||
        std::fabs(gradientLength - 1.0F) > 1.0e-4F)
    {
        return 20;
    }

    // Facing the open sky gives the full irradiance, facing away gives none.
    const auto facingSky =
        EvaluateSkyIrradiance(
            channelSky.sky,
            channelSky.sky.gradient);
    const auto facingAway =
        EvaluateSkyIrradiance(
            channelSky.sky,
            channelSky.sky.gradient * -1.0F);

    if (std::fabs(facingSky.z - 0.5F) > 1.0e-5F ||
        facingAway.x != 0.0F ||
        facingAway.z != 0.0F)
    {
        return 21;
    }

    // An occluder removes the sky channel, as it removes the legacy term.
    const auto channelBlocked =
        EstimateRadianceCellWithSky(
            key,
            config,
            view,
            DirectionalLight{
                .irradianceScale = 0.0F
            },
            {},
            &skyBlockerRegistry,
            skySettings);

    if (channelBlocked.sky.l0.x != 0.0F ||
        channelBlocked.sky.l0.y != 0.0F ||
        channelBlocked.sky.l0.z != 0.0F)
    {
        return 22;
    }

    // The historical entry point still folds the sky into the L1 at transport.
    const auto legacySky =
        EstimateRadianceCell(
            key,
            config,
            view,
            DirectionalLight{
                .irradianceScale = 0.0F
            },
            {},
            nullptr,
            skySettings);

    if (legacySky.l0.z <= 0.0F)
    {
        return 23;
    }

    // Without a sky summary nothing lands in the sky channel.
    const auto noSky =
        EstimateRadianceCellWithSky(
            key,
            config,
            view,
            stellar,
            {},
            nullptr);

    if (noSky.sky.l0.x != 0.0F ||
        noSky.sky.l0.y != 0.0F ||
        noSky.sky.l0.z != 0.0F)
    {
        return 24;
    }

    AlwaysHitProvider blocker;
    VisibilityRegistry blockerRegistry;
    blockerRegistry.Register(blocker);

    const auto blocked =
        EstimateRadianceCell(
            key,
            config,
            view,
            stellar,
            std::span<const ResolvedLocalLight>(
                &lamp,
                1U),
            &blockerRegistry);

    // Ambient remains, but stellar/local directional energy is suppressed.
    if (blocked.l1x.x != 0.0F ||
        blocked.l1y.x != 0.0F ||
        blocked.l1z.x != 0.0F ||
        blocked.l0.x >= noLocal.l0.x)
    {
        return 6;
    }

    return 0;
}
