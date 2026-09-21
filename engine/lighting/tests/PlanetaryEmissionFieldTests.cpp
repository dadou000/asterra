#include <orbit/lighting/PlanetaryEmissionField.hpp>

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
            .sourceStableId = 10U,
            .nodeIndex = 1U,
            .positionInFrameMeters =
                {10.0, 0.0, 0.0},
            .integratedRadianceArea =
                {4.0F, 1.0F, 0.0F},
            .radiantImportance = 4.5,
            .samplingProbability = 0.5F,
            .estimatorWeight = 1.0F
        },
        EmissiveSampledEmitter{
            .sourceStableId = 11U,
            .nodeIndex = 2U,
            .positionInFrameMeters =
                {0.0, 10.0, 0.0},
            .integratedRadianceArea =
                {0.0F, 2.0F, 6.0F},
            .radiantImportance = 3.0,
            .samplingProbability = 0.5F,
            .estimatorWeight = 1.0F
        }
    };

    const auto field =
        BuildPlanetaryEmissionField(
            frame,
            body,
            {},
            7U,
            emitters,
            {
                .baseWidth = 8U,
                .baseHeight = 4U,
                .maximumLevels = 8U
            });

    if (field.levels.empty() ||
        field.levels.back().width != 1U ||
        field.levels.back().height != 1U)
    {
        return 1;
    }

    const auto baseTotal =
        TotalPlanetaryIntegratedRadianceArea(
            field.levels.front());

    for (const auto& level : field.levels)
    {
        const auto total =
            TotalPlanetaryIntegratedRadianceArea(
                level);

        if (std::abs(total.x - baseTotal.x) >
                1.0e-5F ||
            std::abs(total.y - baseTotal.y) >
                1.0e-5F ||
            std::abs(total.z - baseTotal.z) >
                1.0e-5F)
        {
            return 2;
        }
    }

    const auto* equatorial =
        SamplePlanetaryEmission(
            field,
            0U,
            {1.0, 0.0, 0.0});

    if (equatorial == nullptr ||
        equatorial->
            integratedRadianceArea.x <=
            0.0F)
    {
        return 3;
    }

    const auto& global =
        field.levels.back().
            cells.front();

    if (global.contributingEmitters !=
            emitters.size() ||
        std::abs(
            global.integratedRadianceArea.z -
            6.0F) >
            1.0e-5F)
    {
        return 4;
    }

    return 0;
}
