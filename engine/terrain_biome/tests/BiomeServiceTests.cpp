#include <orbit/terrain_biome/BiomeService.hpp>
#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_material_column/SurfaceResolver.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using namespace orbit;
using namespace orbit::terrain_biome;
using namespace orbit::terrain_material_column;

[[noreturn]] void Fail(
    const std::string& message)
{
    std::cerr
        << "M19 failure: "
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

void RequireNear(
    const f64 a,
    const f64 b,
    const f64 tolerance,
    const std::string& message)
{
    if (std::abs(a - b) >
        tolerance)
    {
        Fail(
            message +
            " (" +
            std::to_string(a) +
            " vs " +
            std::to_string(b) +
            ")");
    }
}

[[nodiscard]] universe::BodyId
Body()
{
    return {
        .high = 0x4d3139424f445930ULL,
        .low = 0x0000000000000001ULL
    };
}

[[nodiscard]] BiomeId
ForestId()
{
    return {
        .high = 0x4d313942494f4d45ULL,
        .low = 0x0000000000000001ULL
    };
}

[[nodiscard]] BiomeId
DuneId()
{
    return {
        .high = 0x4d313942494f4d45ULL,
        .low = 0x0000000000000002ULL
    };
}

BiomeDefinition Forest()
{
    return {
        .id = ForestId(),
        .name = "Temperate Forest",
        .placement = {
            .minimumResolvedWeight = 0.25F,
            .enabled = true
        },
        .surface = {
            .materialInfluence = 1.0F
        },
        .scatter = {
            .densityMultiplier = 1.35F
        },
        .processModifiers = {
            .hydraulicErosion = 1.0F,
            .thermalTransport = 1.0F,
            .aeolianTransport = 0.8F,
            .glacialErosion = 1.0F,
            .coastalErosion = 1.0F,
            .chemicalWeathering = 1.2F
        }
    };
}

BiomeDefinition Dunes()
{
    return {
        .id = DuneId(),
        .name = "Dune Field",
        .placement = {
            .minimumResolvedWeight = 0.10F,
            .enabled = true
        }
    };
}

const ResolvedBiomeWeight* FindResolved(
    const std::vector<ResolvedBiomeWeight>& weights,
    const BiomeId id)
{
    for (const auto& weight : weights)
    {
        if (weight.id == id)
        {
            return &weight;
        }
    }

    return nullptr;
}

void TestBaseOnlyPlanetIsValid()
{
    BiomeService service(
        Body());

    const auto definitions =
        service.Definitions();

    Require(
        definitions.size() == 1U,
        "A new rocky planet must contain exactly one BaseBiome.");

    Require(
        definitions.front().id ==
            service.BaseBiome().id,
        "The only initial definition is not the BaseBiome.");

    const auto weights =
        service.Resolve({});

    Require(
        weights.size() == 1U &&
            weights.front().base,
        "Base-only biome resolution must produce exactly one fallback entry.");

    RequireNear(
        weights.front().weight,
        1.0,
        0.0,
        "Base-only planet must resolve to full BaseBiome coverage.");
}

void TestResidualFallbackAndThreshold()
{
    BiomeService service(
        Body());

    service.UpsertBiome(
        Forest());

    const std::array<
        BiomeWeightContribution,
        1>
        weak{{
            {
                .id = ForestId(),
                .weight = 0.20F
            }
        }};

    const auto below =
        service.Resolve(
            weak);

    Require(
        below.size() == 1U &&
            below.front().base,
        "A biome below its eligibility threshold must fall back to BaseBiome.");

    RequireNear(
        below.front().weight,
        1.0,
        0.0,
        "Below-threshold biome left undefined coverage.");

    const std::array<
        BiomeWeightContribution,
        1>
        strong{{
            {
                .id = ForestId(),
                .weight = 0.40F
            }
        }};

    const auto resolved =
        service.Resolve(
            strong);

    Require(
        resolved.size() == 2U,
        "Eligible biome should coexist with residual BaseBiome coverage.");

    const auto* base =
        FindResolved(
            resolved,
            service.BaseBiome().id);

    const auto* forest =
        FindResolved(
            resolved,
            ForestId());

    Require(
        base != nullptr &&
            base->base &&
            forest != nullptr &&
            !forest->base,
        "Resolved coverage lost base/optional biome identity.");

    RequireNear(
        base->weight,
        0.60,
        1.0e-6,
        "BaseBiome is not the residual uncovered weight.");

    RequireNear(
        forest->weight,
        0.40,
        1.0e-6,
        "Eligible biome weight changed unexpectedly.");
}

void TestOverSubscribedBiomesNormalize()
{
    BiomeService service(
        Body());

    service.UpsertBiome(
        Forest());

    service.UpsertBiome(
        Dunes());

    const std::array<
        BiomeWeightContribution,
        2>
        contributions{{
            {
                .id = ForestId(),
                .weight = 0.80F
            },
            {
                .id = DuneId(),
                .weight = 0.80F
            }
        }};

    const auto resolved =
        service.Resolve(
            contributions);

    Require(
        resolved.size() == 3U,
        "Oversubscribed optional biomes must retain exactly one BaseBiome entry.");

    const auto* base =
        FindResolved(
            resolved,
            service.BaseBiome().id);

    const auto* forest =
        FindResolved(
            resolved,
            ForestId());

    const auto* dune =
        FindResolved(
            resolved,
            DuneId());

    Require(
        base != nullptr &&
            forest != nullptr &&
            dune != nullptr,
        "Oversubscribed biome resolution lost a definition.");

    RequireNear(
        base->weight,
        0.0,
        1.0e-6,
        "BaseBiome must fall to zero only when optional coverage consumes all weight.");

    RequireNear(
        forest->weight,
        0.5,
        1.0e-6,
        "Oversubscribed forest weight was not normalized.");

    RequireNear(
        dune->weight,
        0.5,
        1.0e-6,
        "Oversubscribed dune weight was not normalized.");
}

void TestRemovingBiomeCannotLeaveUndefinedTerrain()
{
    BiomeService service(
        Body());

    service.UpsertBiome(
        Forest());

    const BiomeId baseId =
        service.BaseBiome().id;

    Require(
        !service.RemoveBiome(
            baseId),
        "BaseBiome must not be removable.");

    Require(
        service.RemoveBiome(
            ForestId()),
        "Optional biome removal failed.");

    const std::array<
        BiomeWeightContribution,
        1>
        stale{{
            {
                .id = ForestId(),
                .weight = 0.95F
            }
        }};

    const auto resolved =
        service.Resolve(
            stale);

    Require(
        resolved.size() == 1U &&
            resolved.front().id ==
                baseId &&
            resolved.front().base,
        "Deleting a biome left stale/undefined terrain coverage.");

    RequireNear(
        resolved.front().weight,
        1.0,
        0.0,
        "Deleted biome did not return coverage to BaseBiome.");
}

terrain_geology::GeologicalMaterialLibrary
MakeGeology()
{
    terrain_geology::GeologicalMaterialLibrary geology;

    geology.Upsert({
        .id =
            terrain_geology::
                reference_rock::
                    Basalt,
        .name = "M19 basalt",
        .hardness = 0.9F,
        .cohesion = 0.9F,
        .hydraulicErodibility = 0.1F,
        .aeolianErodibility = 0.05F,
        .permeability = 0.1F,
        .chemicalWeatherability = 0.2F,
        .fractureTendency = 0.3F,
        .density = 3'000.0F
    });

    geology.Upsert({
        .id =
            terrain_geology::
                reference_rock::
                    Granite,
        .name = "M19 granite",
        .hardness = 0.95F,
        .cohesion = 0.95F,
        .hydraulicErodibility = 0.06F,
        .aeolianErodibility = 0.03F,
        .permeability = 0.05F,
        .chemicalWeatherability = 0.15F,
        .fractureTendency = 0.25F,
        .density = 2'700.0F
    });

    return geology;
}

MaterialColumnCell BareRock(
    const terrain_geology::RockTypeId rock)
{
    return {
        .bedrockHeightMeters = 100.0F,
        .referenceBedrockHeightMeters = 100.0F,
        .bedrockMaterial = rock,
        .regolithMeters = 0.0F,
        .soilMeters = 0.0F,
        .sandMeters = 0.0F,
        .debrisMeters = 0.0F,
        .moisture = 0.25F,
        .temporaryScalar = 0.0F
    };
}

void TestBiomeAssetIsIndependentFromGeology()
{
    auto geology =
        MakeGeology();

    const auto basalt =
        BareRock(
            terrain_geology::
                reference_rock::
                    Basalt);

    const auto granite =
        BareRock(
            terrain_geology::
                reference_rock::
                    Granite);

    const auto basaltSurface =
        ResolveSurface(
            basalt,
            SampleColumnGeology(
                basalt,
                geology));

    const auto graniteSurface =
        ResolveSurface(
            granite,
            SampleColumnGeology(
                granite,
                geology));

    Require(
        basaltSurface.substrateRock !=
            graniteSurface.substrateRock,
        "M19 geology-independence fixture did not use distinct rocks.");

    BiomeService service(
        Body());

    service.UpsertBiome(
        Forest());

    const std::array<
        BiomeWeightContribution,
        1>
        contribution{{
            {
                .id = ForestId(),
                .weight = 0.70F
            }
        }};

    const auto basaltBiomes =
        service.Resolve(
            contribution);

    const auto graniteBiomes =
        service.Resolve(
            contribution);

    const auto* basaltForest =
        FindResolved(
            basaltBiomes,
            ForestId());

    const auto* graniteForest =
        FindResolved(
            graniteBiomes,
            ForestId());

    Require(
        basaltForest != nullptr &&
            graniteForest != nullptr &&
            basaltForest->id ==
                graniteForest->id,
        "One biome asset could not be reused across distinct geological substrates.");

    Require(
        service.Definitions().size() ==
            2U,
        "Reusing a biome across geology unexpectedly duplicated the biome definition.");
}
} // namespace

int main()
{
    TestBaseOnlyPlanetIsValid();
    TestResidualFallbackAndThreshold();
    TestOverSubscribedBiomesNormalize();
    TestRemovingBiomeCannotLeaveUndefinedTerrain();
    TestBiomeAssetIsIndependentFromGeology();

    std::cout
        << "Orbit M19 biome service tests passed.\n";

    return EXIT_SUCCESS;
}
