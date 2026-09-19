#include <orbit/terrain_scatter/DeterministicScatter.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using namespace orbit;
using namespace orbit::terrain_scatter;
using namespace orbit::terrain_biome;
using namespace orbit::terrain_material_column;

[[noreturn]] void Fail(
    const std::string& message)
{
    std::cerr
        << "M22 failure: "
        << message
        << '\n';

    std::exit(
        EXIT_FAILURE);
}

void Require(
    const bool condition,
    const std::string& message)
{
    if (!condition)
    {
        Fail(message);
    }
}

BiomeScatterRuleId RuleId(
    const u64 low)
{
    return {
        .high = 0x4D32325343415454ULL,
        .low = low
    };
}

world::PlanetId PlanetId()
{
    return {
        .high = 0x4D3232504C414E45ULL,
        .low = 1U
    };
}

BiomeScatterLayerRule TreeRule()
{
    return {
        .id = RuleId(1U),
        .kind = BiomeScatterKind::Tree,
        .densityPerSquareMeter = 1.0F,
        .minimumSpacingMeters = 4.0F,
        .seedSalt = 77U,
        .compatibleExposed = BiomeExposedMaterialMask::Soil,
        .requiresSoil = true,
        .minimumSoilDepthMeters = 0.20F,
        .minimumSlopeDegrees = 0.0F,
        .maximumSlopeDegrees = 35.0F,
        .slopeFalloffDegrees = 10.0F,
        .minimumMoisture = 0.20F,
        .maximumMoisture = 0.90F,
        .moistureFalloff = 0.10F,
        .minimumScale = 0.8F,
        .maximumScale = 1.2F
    };
}

BiomeScatterLayerRule StoneRule()
{
    return {
        .id = RuleId(2U),
        .kind = BiomeScatterKind::Stone,
        .densityPerSquareMeter = 1.0F,
        .minimumSpacingMeters = 2.0F,
        .seedSalt = 91U,
        .compatibleExposed =
            BiomeExposedMaterialMask::All,
        .requiresSoil = false,
        .minimumSoilDepthMeters = 0.0F,
        .minimumSlopeDegrees = 0.0F,
        .maximumSlopeDegrees = 90.0F,
        .minimumMoisture = 0.0F,
        .maximumMoisture = 1.0F,
        .minimumScale = 0.5F,
        .maximumScale = 1.5F
    };
}

ScatterPageRequest Request(
    const BiomeScatterLayerRule& rule)
{
    return {
        .identity = {
            .planet = PlanetId(),
            .tile = {
                .face = world::CubeFace::PositiveX,
                .level = 12U,
                .x = 812U,
                .y = 437U
            },
            .sourceRevision = 41U,
            .scatterRevision = 9U,
            .generationSeed = 0xA57E22A60022ULL
        },
        .gridResolution = 20U,
        .cellSizeMeters =
            rule.minimumSpacingMeters,
        .rule = rule,
        .biomeDensityMultiplier = 1.0F
    };
}

std::vector<ScatterCellInput> SoilField(
    const u32 resolution)
{
    return std::vector<ScatterCellInput>(
        static_cast<std::size_t>(
            resolution) *
            resolution,
        {
            .biomeWeight = 1.0F,
            .exposedMaterial =
                ExposedSurfaceKind::Soil,
            .slopeDegrees = 10.0F,
            .soilDepthMeters = 0.8F,
            .moisture = 0.6F,
            .exclusionMask = 0.0F,
            .authoredDensity = 1.0F
        });
}

void RequireSameInstances(
    const std::vector<DerivedScatterInstance>& a,
    const std::vector<DerivedScatterInstance>& b,
    const std::string& message)
{
    if (a.size() != b.size())
    {
        Fail(message + " (count)");
    }

    for (std::size_t index = 0U;
         index < a.size();
         ++index)
    {
        const auto& left = a[index];
        const auto& right = b[index];

        if (left.id != right.id ||
            left.kind != right.kind ||
            left.localOffsetMeters.x !=
                right.localOffsetMeters.x ||
            left.localOffsetMeters.y !=
                right.localOffsetMeters.y ||
            left.yawRadians !=
                right.yawRadians ||
            left.uniformScale !=
                right.uniformScale ||
            left.cellX != right.cellX ||
            left.cellY != right.cellY)
        {
            Fail(message + " (instance)");
        }
    }
}

void TestExactRegenerationIdentity()
{
    const auto rule =
        TreeRule();

    const auto request =
        Request(rule);

    const auto cells =
        SoilField(
            request.gridResolution);

    const auto first =
        GenerateDeterministicScatter(
            request,
            cells);

    const auto second =
        GenerateDeterministicScatter(
            request,
            cells);

    Require(
        !first.empty(),
        "Deterministic fixture generated no tree candidates.");

    RequireSameInstances(
        first,
        second,
        "Same seed/page/revision reshuffled scatter after stream-in.");
}

void TestRevisionChangesDerivedPopulation()
{
    const auto rule =
        TreeRule();

    auto request =
        Request(rule);

    const auto cells =
        SoilField(
            request.gridResolution);

    const auto original =
        GenerateDeterministicScatter(
            request,
            cells);

    ++request.identity.scatterRevision;

    const auto changed =
        GenerateDeterministicScatter(
            request,
            cells);

    bool differs =
        original.size() !=
        changed.size();

    if (!differs)
    {
        for (std::size_t i = 0U;
             i < original.size();
             ++i)
        {
            if (original[i].id !=
                changed[i].id)
            {
                differs = true;
                break;
            }
        }
    }

    Require(
        differs,
        "Changing scatter revision did not invalidate the derived population.");
}

void TestBareRockRejectsSoilVegetation()
{
    const auto rule =
        TreeRule();

    const auto request =
        Request(rule);

    auto cells =
        SoilField(
            request.gridResolution);

    for (auto& cell : cells)
    {
        cell.exposedMaterial =
            ExposedSurfaceKind::Bedrock;

        // Deliberately stale/non-zero. M22 must trust physical exposure first.
        cell.soilDepthMeters = 2.0F;
    }

    const auto instances =
        GenerateDeterministicScatter(
            request,
            cells);

    Require(
        instances.empty(),
        "Bare rock accepted soil-dependent trees.");
}

void TestBareRockAcceptsNonSoilStone()
{
    const auto rule =
        StoneRule();

    const auto request =
        Request(rule);

    std::vector<ScatterCellInput> cells(
        static_cast<std::size_t>(
            request.gridResolution) *
            request.gridResolution,
        {
            .biomeWeight = 1.0F,
            .exposedMaterial =
                ExposedSurfaceKind::Bedrock,
            .slopeDegrees = 40.0F,
            .soilDepthMeters = 0.0F,
            .moisture = 0.1F,
            .exclusionMask = 0.0F,
            .authoredDensity = 1.0F
        });

    const auto instances =
        GenerateDeterministicScatter(
            request,
            cells);

    Require(
        !instances.empty(),
        "Non-soil stone scatter was incorrectly rejected on bare rock.");
}

void TestExclusionAndAuthoredDensity()
{
    const auto rule =
        StoneRule();

    const auto request =
        Request(rule);

    std::vector<ScatterCellInput> cells(
        static_cast<std::size_t>(
            request.gridResolution) *
            request.gridResolution,
        {
            .biomeWeight = 1.0F,
            .exposedMaterial =
                ExposedSurfaceKind::Bedrock,
            .slopeDegrees = 10.0F,
            .soilDepthMeters = 0.0F,
            .moisture = 0.3F,
            .exclusionMask = 1.0F,
            .authoredDensity = 1.0F
        });

    Require(
        GenerateDeterministicScatter(
            request,
            cells).
            empty(),
        "Full exclusion mask still emitted derived scatter.");

    for (auto& cell : cells)
    {
        cell.exclusionMask = 0.0F;
        cell.authoredDensity = 0.0F;
    }

    Require(
        GenerateDeterministicScatter(
            request,
            cells).
            empty(),
        "Zero authored density still emitted derived scatter.");
}

void TestMinimumSpacing()
{
    const auto rule =
        TreeRule();

    const auto request =
        Request(rule);

    const auto cells =
        SoilField(
            request.gridResolution);

    const auto instances =
        GenerateDeterministicScatter(
            request,
            cells);

    const f64 minimumSquared =
        static_cast<f64>(
            rule.minimumSpacingMeters) *
        rule.minimumSpacingMeters;

    for (std::size_t a = 0U;
         a < instances.size();
         ++a)
    {
        for (std::size_t b = a + 1U;
             b < instances.size();
             ++b)
        {
            const auto delta =
                instances[a].
                    localOffsetMeters -
                instances[b].
                    localOffsetMeters;

            const f64 distanceSquared =
                delta.x * delta.x +
                delta.y * delta.y;

            if (distanceSquared <
                minimumSquared -
                    1.0e-8)
            {
                Fail(
                    "M22 Poisson-like neighbor rejection violated minimum spacing.");
            }
        }
    }
}
} // namespace

int main()
{
    TestExactRegenerationIdentity();
    TestRevisionChangesDerivedPopulation();
    TestBareRockRejectsSoilVegetation();
    TestBareRockAcceptsNonSoilStone();
    TestExclusionAndAuthoredDensity();
    TestMinimumSpacing();

    std::cout
        << "Orbit M22 deterministic scatter CPU tests passed.\n";

    return EXIT_SUCCESS;
}
