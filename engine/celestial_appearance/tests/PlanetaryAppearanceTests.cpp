#include <orbit/celestial_appearance/PlanetaryAppearance.hpp>

#include <cmath>

namespace
{
class TestTerrain final :
    public orbit::terrain::TerrainSource
{
public:
    explicit TestTerrain(
        const orbit::u64 revision)
        : revision_(revision)
    {
    }

    orbit::terrain::TerrainSample Sample(
        const orbit::terrain::TerrainQuery& query)
        const noexcept override
    {
        const auto d =
            orbit::math::Normalize(
                query.unitDirection);

        orbit::terrain::TerrainSample result;
        result.elevationMeters =
            1200.0 * d.y;
        result.coarseElevationMeters =
            result.elevationMeters;
        result.climate.temperatureC =
            static_cast<orbit::f32>(
                20.0 - 45.0 *
                std::abs(d.y));

        if (d.x < 0.0)
        {
            result.standingWaterDepthMeters =
                3000.0;
            result.biomes.ocean = 1.0F;
            result.biomes.grassland = 0.0F;
        }
        else
        {
            result.biomes.desert =
                d.y < 0.0 ? 1.0F : 0.0F;
            result.biomes.tundra =
                d.y >= 0.0 ? 1.0F : 0.0F;
            result.biomes.grassland = 0.0F;
        }

        return result;
    }

    orbit::u64 Revision() const noexcept override
    {
        return revision_;
    }

    orbit::terrain::TerrainGenerationRevisions
    GenerationRevisions() const noexcept override
    {
        return {
            .geology = revision_,
            .climate = revision_ + 1U,
            .biome = revision_ + 2U,
            .water = revision_ + 3U
        };
    }

private:
    orbit::u64 revision_{0};
};
} // namespace

int main()
{
    TestTerrain terrain(100);

    const orbit::celestial_appearance::AppearanceConfig
        config{
            .faceResolution = 9,
            .footprintScale = 1.5
        };

    const auto product =
        orbit::celestial_appearance::
            BuildPlanetaryAppearance(
                terrain,
                1.0e6,
                config);

    if (product.texels.size() !=
            6U * 9U * 9U ||
        product.faceResolution != 9U ||
        product.sampleFootprintMeters <= 0.0)
    {
        return 1;
    }

    const auto fingerprint =
        orbit::celestial_appearance::
            PlanetaryAppearanceFingerprint(
                terrain,
                1.0e6,
                config);

    if (fingerprint !=
        product.fingerprint)
    {
        return 2;
    }

    bool sawOcean = false;
    bool sawIce = false;
    bool sawDry = false;

    for (const auto& texel :
         product.texels)
    {
        const double n =
            orbit::math::Length(
                texel.normal);

        if (!std::isfinite(n) ||
            std::abs(n - 1.0) >
                1.0e-5 ||
            texel.roughness < 0.0F ||
            texel.roughness > 1.0F)
        {
            return 3;
        }

        sawOcean =
            sawOcean ||
            texel.oceanMask > 0.5F;
        sawIce =
            sawIce ||
            texel.iceMask > 0.1F;
        sawDry =
            sawDry ||
            texel.oceanMask < 0.5F;

        if (orbit::math::LengthSquared(
                texel.emissionLinear) !=
            0.0F)
        {
            return 4;
        }
    }

    if (!sawOcean ||
        !sawIce ||
        !sawDry)
    {
        return 5;
    }

    TestTerrain revised(101);

    if (orbit::celestial_appearance::
            PlanetaryAppearanceFingerprint(
                revised,
                1.0e6,
                config) ==
        product.fingerprint)
    {
        return 6;
    }

    const orbit::celestial_appearance::AppearanceConfig
        dryConfig{
            .faceResolution = 9,
            .footprintScale = 1.5,
            .standingWaterEnabled = false
        };
    const auto dryProduct =
        orbit::celestial_appearance::
            BuildPlanetaryAppearance(
                terrain,
                1.0e6,
                dryConfig);

    for (const auto& texel : dryProduct.texels)
    {
        if (texel.oceanMask != 0.0F ||
            texel.waterDepthMeters != 0.0F)
        {
            return 7;
        }
    }

    if (dryProduct.fingerprint ==
        product.fingerprint)
    {
        return 8;
    }

    return 0;
}
