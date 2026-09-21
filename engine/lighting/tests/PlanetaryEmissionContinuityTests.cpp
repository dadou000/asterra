#include <orbit/lighting/PlanetaryEmissionService.hpp>
#include <orbit/lighting/RadianceEstimator.hpp>

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

    const RuntimeEmissiveSurface source{
        .geometry = {
            .frame = frame,
            .body = body,
            .stableId = 400U,
            .contentRevision = 8U,
            .originInFrameMeters =
                {10.0, -1.0, -1.0},
            .axisUInFrameMeters =
                {0.0, 2.0, 0.0},
            .axisVInFrameMeters =
                {0.0, 0.0, 2.0}
        },
        .contentRevision = 8U,
        .width = 2U,
        .height = 2U,
        .giRadiance = {
            {8.0F, 1.0F, 0.2F},
            {8.0F, 1.0F, 0.2F},
            {0.5F, 2.0F, 5.0F},
            {0.5F, 2.0F, 5.0F}
        }
    };

    const auto hierarchy =
        BuildEmissiveHierarchy(
            source.Grid(),
            {
                .leafTileWidth = 1U,
                .leafTileHeight = 1U
            });

    LightingView view;
    view.frame = frame;
    view.body = body;
    view.cameraPositionInFrameMeters =
        {0.0, 0.0, 0.0};
    view.gpuOriginInFrameMeters =
        {};
    view.verticalFovRadians = 1.0F;

    const auto localSamples =
        BuildEmissiveSampleSet(
            hierarchy,
            view,
            1920U,
            1080U,
            {
                .subdivisionProjectedPixels =
                    1.0F,
                .maximumSamples = 64U
            });

    if (!localSamples.EnergyPartitionValid())
    {
        return 1;
    }

    math::Float3 localIntegrated{};

    for (const auto& emitter :
         localSamples.emitters)
    {
        localIntegrated.x +=
            emitter.integratedRadianceArea.x;
        localIntegrated.y +=
            emitter.integratedRadianceArea.y;
        localIntegrated.z +=
            emitter.integratedRadianceArea.z;
    }

    PlanetaryEmissionService service({
        .baseWidth = 16U,
        .baseHeight = 8U,
        .maximumLevels = 5U
    });

    if (!service.Upsert(source))
    {
        return 2;
    }

    const auto* field =
        service.Resolve(
            frame,
            body,
            {});

    if (field == nullptr ||
        field->levels.empty())
    {
        return 3;
    }

    const auto regionalIntegrated =
        TotalPlanetaryIntegratedRadianceArea(
            field->levels.front());

    if (std::abs(
            localIntegrated.x -
            regionalIntegrated.x) >
            1.0e-5F ||
        std::abs(
            localIntegrated.y -
            regionalIntegrated.y) >
            1.0e-5F ||
        std::abs(
            localIntegrated.z -
            regionalIntegrated.z) >
            1.0e-5F)
    {
        return 4;
    }

    for (const auto& level :
         field->levels)
    {
        const auto levelIntegrated =
            TotalPlanetaryIntegratedRadianceArea(
                level);

        if (std::abs(
                levelIntegrated.x -
                localIntegrated.x) >
                1.0e-5F ||
            std::abs(
                levelIntegrated.y -
                localIntegrated.y) >
                1.0e-5F ||
            std::abs(
                levelIntegrated.z -
                localIntegrated.z) >
                1.0e-5F)
        {
            return 5;
        }
    }

    return 0;
}
