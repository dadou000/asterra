#include <orbit/lighting/PlanetaryEmissionAppearance.hpp>

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

    const std::array emitters{
        EmissiveSampledEmitter{
            .sourceStableId = 1U,
            .nodeIndex = 0U,
            .positionInFrameMeters =
                {100.0, 0.0, 0.0},
            .integratedRadianceArea =
                {50.0F, 5.0F, 1.0F},
            .radiantImportance = 20.0,
            .samplingProbability = 1.0F,
            .estimatorWeight = 1.0F
        }
    };

    const auto field =
        BuildPlanetaryEmissionField(
            frame,
            body,
            {},
            11U,
            emitters,
            {
                .baseWidth = 8U,
                .baseHeight = 4U,
                .maximumLevels = 4U
            });

    celestial_appearance::
        PlanetaryAppearanceProduct appearance;
    appearance.faceResolution = 3U;
    appearance.fingerprint = 123U;
    appearance.texels.resize(
        6U * 3U * 3U);

    const u64 before =
        appearance.fingerprint;

    ApplyPlanetaryEmissionField(
        appearance,
        field,
        100.0,
        0U);

    bool sawEmission = false;
    bool sawRedDominant = false;

    for (const auto& texel :
         appearance.texels)
    {
        sawEmission |=
            texel.emissionLinear.x > 0.0F ||
            texel.emissionLinear.y > 0.0F ||
            texel.emissionLinear.z > 0.0F;

        sawRedDominant |=
            texel.emissionLinear.x >
            texel.emissionLinear.y;
    }

    if (!sawEmission ||
        !sawRedDominant ||
        appearance.fingerprint == before)
    {
        return 1;
    }

    const auto fineTotal =
        TotalPlanetaryIntegratedRadianceArea(
            field.levels.front());

    const auto coarseTotal =
        TotalPlanetaryIntegratedRadianceArea(
            field.levels.back());

    if (std::abs(
            fineTotal.x -
            coarseTotal.x) >
            1.0e-5F ||
        std::abs(
            fineTotal.y -
            coarseTotal.y) >
            1.0e-5F ||
        std::abs(
            fineTotal.z -
            coarseTotal.z) >
            1.0e-5F)
    {
        return 2;
    }

    const auto fineRadiance =
        EvaluatePlanetaryEmissionRadiance(
            field,
            0U,
            {1.0, 0.0, 0.0},
            100.0);

    if (fineRadiance.x <= 0.0F)
    {
        return 3;
    }

    return 0;
}
