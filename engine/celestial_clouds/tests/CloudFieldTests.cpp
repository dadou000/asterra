#include <orbit/celestial_clouds/CloudField.hpp>
#include <orbit/terrain/TerrainSource.hpp>

#include <cmath>

namespace
{
class ClimateSource final : public orbit::terrain::TerrainSource
{
public:
    orbit::terrain::TerrainSample Sample(
        const orbit::terrain::TerrainQuery& query) const noexcept override
    {
        const auto d =
            orbit::math::Normalize(
                query.unitDirection);

        return {
            .climate = {
                .temperatureC = 15.0F,
                .humidity =
                    static_cast<orbit::f32>(
                        std::clamp(
                            0.55 + 0.35 * d.y,
                            0.0,
                            1.0)),
                .precipitation =
                    static_cast<orbit::f32>(
                        std::clamp(
                            0.45 + 0.30 * d.x,
                            0.0,
                            1.0)),
                .continentality = 0.5F
            }
        };
    }

    orbit::terrain::TerrainGenerationRevisions
    GenerationRevisions() const noexcept override
    {
        return {
            .climate = 17U
        };
    }
};
}

int main()
{
    using namespace orbit;
    using namespace orbit::celestial_clouds;

    ClimateSource climate;

    const CloudLayerParameters layer{
        .semanticIdHigh = 1U,
        .semanticIdLow = 2U,
        .sourceModel =
            CloudSourceModel::
                ClimateProcedural,
        .baseAltitudeMeters = 1500.0,
        .topAltitudeMeters = 6500.0,
        .peakOpticalDepth = 6.0,
        .seed = 42U
    };

    const CloudFieldConfig config{
        .faceResolution = 17U,
        .footprintScale = 2.0,
        .timeQuantumMicroseconds =
            1'000'000
    };

    const auto a =
        BuildCloudField(
            &climate,
            nullptr,
            6.371e6,
            {layer},
            {},
            config);

    const auto b =
        BuildCloudField(
            &climate,
            nullptr,
            6.371e6,
            {layer},
            {},
            config);

    if (a.fingerprint != b.fingerprint ||
        a.climateRevision != 17U ||
        a.layers.size() != 1U)
    {
        return 1;
    }

    const auto north =
        a.Sample({0.0, 1.0, 0.0});
    const auto south =
        a.Sample({0.0, -1.0, 0.0});

    if (north.coverage ==
        south.coverage)
    {
        return 2;
    }

    const auto later =
        BuildCloudField(
            &climate,
            nullptr,
            6.371e6,
            {layer},
            {
                .microsecondsFromEpoch =
                    2'000'000
            },
            config);

    if (later.fingerprint ==
        a.fingerprint)
    {
        return 3;
    }

    const f64 cloudyShadow =
        CloudShadowTransmittanceAtSurface(
            a,
            6.371e6,
            {0.0, 1.0, 0.0},
            math::Normalize(
                math::Double3{
                    0.3, 0.9, 0.1}));

    if (!(cloudyShadow >= 0.0) ||
        !(cloudyShadow <= 1.0))
    {
        return 4;
    }

    auto noShadowLayer =
        layer;
    noShadowLayer.shadowParticipation =
        false;

    const auto noShadow =
        BuildCloudField(
            &climate,
            nullptr,
            6.371e6,
            {noShadowLayer},
            {},
            config);

    if (CloudShadowTransmittanceAtSurface(
            noShadow,
            6.371e6,
            {0.0, 1.0, 0.0},
            {0.0, 1.0, 0.0}) != 1.0)
    {
        return 5;
    }

    return 0;
}
