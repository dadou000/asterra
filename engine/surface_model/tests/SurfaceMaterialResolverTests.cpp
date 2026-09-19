#include <orbit/surface_model/SurfaceMaterialResolver.hpp>
#include <orbit/terrain_material_column/SurfaceResolver.hpp>
#include <orbit/terrain_render/SurfaceMaterial.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
using namespace orbit;
using namespace orbit::surface_model;
using namespace orbit::terrain_biome;
using namespace orbit::terrain_material_column;

[[noreturn]] void Fail(
    const std::string& message)
{
    std::cerr
        << "M21 failure: "
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

[[nodiscard]] universe::BodyId Body()
{
    return {
        .high = 0x4d32315355524641ULL,
        .low = 0x4345424c454e4401ULL
    };
}

[[nodiscard]] BiomeId ForestId()
{
    return {
        .high = 0x4d3231464f524553ULL,
        .low = 0x5400000000000001ULL
    };
}

[[nodiscard]] BiomeId DesertId()
{
    return {
        .high = 0x4d32314445534552ULL,
        .low = 0x5400000000000001ULL
    };
}

terrain_geology::GeologicalMaterialLibrary Geology()
{
    terrain_geology::GeologicalMaterialLibrary geology;

    geology.Upsert({
        .id =
            terrain_geology::
                reference_rock::
                    Basalt,
        .name = "M21 basalt",
        .hardness = 0.92F,
        .cohesion = 0.88F,
        .hydraulicErodibility = 0.12F,
        .aeolianErodibility = 0.04F,
        .permeability = 0.08F,
        .chemicalWeatherability = 0.20F,
        .fractureTendency = 0.35F,
        .density = 3'000.0F
    });

    return geology;
}

MaterialColumnCell Cell(
    const f32 soil,
    const f32 sand,
    const f32 moisture)
{
    return {
        .bedrockHeightMeters = 100.0F,
        .referenceBedrockHeightMeters = 100.0F,
        .bedrockMaterial =
            terrain_geology::
                reference_rock::
                    Basalt,
        .regolithMeters = 0.0F,
        .soilMeters = soil,
        .sandMeters = sand,
        .debrisMeters = 0.0F,
        .moisture = moisture,
        .temporaryScalar = 0.0F
    };
}

ExposedSurfaceState Surface(
    const MaterialColumnCell& cell)
{
    auto geology =
        Geology();

    return
        ResolveSurface(
            cell,
            SampleColumnGeology(
                cell,
                geology));
}

std::vector<ResolvedBiomeWeight> FullBiomeWeight(
    const BiomeService& service,
    const BiomeId id)
{
    const std::array<
        BiomeWeightContribution,
        1>
        contribution{{
            {
                .id = id,
                .weight = 1.0F
            }
        }};

    return
        service.Resolve(
            contribution);
}

BiomeDefinition Forest()
{
    BiomeDefinition biome{
        .id = ForestId(),
        .name = "Forest",
        .placement = {
            .minimumResolvedWeight = 0.0F,
            .enabled = true
        },
        .surface = {
            .materialInfluence = 1.0F
        }
    };

    biome.surface.layers = {
        {
            .kind =
                BiomeSurfaceLayerKind::
                    Moss,
            .strength = 0.75F,
            .compatibleExposed =
                BiomeExposedMaterialMask::Bedrock |
                BiomeExposedMaterialMask::Regolith |
                BiomeExposedMaterialMask::Soil,
            .minimumSlopeDegrees = 0.0F,
            .maximumSlopeDegrees = 45.0F,
            .slopeFalloffDegrees = 10.0F,
            .minimumCurvature = -0.5F,
            .maximumCurvature = 1.0F,
            .curvatureFalloff = 0.25F,
            .minimumMoisture = 0.55F,
            .maximumMoisture = 1.0F,
            .moistureFalloff = 0.20F
        },
        {
            .kind =
                BiomeSurfaceLayerKind::
                    Litter,
            .strength = 0.80F,
            .compatibleExposed =
                BiomeExposedMaterialMask::Soil,
            .minimumSlopeDegrees = 0.0F,
            .maximumSlopeDegrees = 30.0F,
            .slopeFalloffDegrees = 10.0F,
            .minimumCurvature = -1.0F,
            .maximumCurvature = 1.0F,
            .curvatureFalloff = 0.0F,
            .minimumMoisture = 0.20F,
            .maximumMoisture = 1.0F,
            .moistureFalloff = 0.10F
        }
    };

    return biome;
}

BiomeDefinition Desert()
{
    BiomeDefinition biome{
        .id = DesertId(),
        .name = "Desert",
        .placement = {
            .minimumResolvedWeight = 0.0F,
            .enabled = true
        },
        .surface = {
            .materialInfluence = 1.0F
        }
    };

    biome.surface.layers = {
        {
            .kind =
                BiomeSurfaceLayerKind::
                    Dust,
            .strength = 0.65F,
            .compatibleExposed =
                BiomeExposedMaterialMask::Bedrock |
                BiomeExposedMaterialMask::Regolith,
            .minimumSlopeDegrees = 0.0F,
            .maximumSlopeDegrees = 55.0F,
            .slopeFalloffDegrees = 15.0F,
            .minimumCurvature = -1.0F,
            .maximumCurvature = 1.0F,
            .curvatureFalloff = 0.0F,
            .minimumMoisture = 0.0F,
            .maximumMoisture = 0.25F,
            .moistureFalloff = 0.15F
        }
    };

    return biome;
}

void TestWetBasaltForestAddsMoss()
{
    BiomeService service(
        Body());

    service.UpsertBiome(
        Forest());

    const auto weights =
        FullBiomeWeight(
            service,
            ForestId());

    const auto blend =
        ResolveSurfaceMaterialBlend(
            Surface(
                Cell(
                    0.0F,
                    0.0F,
                    0.85F)),
            service,
            weights,
            {
                .slopeDegrees = 10.0F,
                .curvature = 0.1F,
                .mossPotential = 1.0F
            });

    Require(
        blend.IsValid(),
        "Wet basalt/forest blend is invalid.");

    Require(
        blend.Weight(
            RenderedSurfaceMaterialKind::
                Moss) >
            0.5F,
        "Wet basalt + forest did not add moss.");

    Require(
        blend.Weight(
            RenderedSurfaceMaterialKind::
                Bedrock) >
            0.0F,
        "Forest biome erased exposed basalt without a full physical overlay.");

    const auto packed =
        terrain_render::
            MakeSurfaceMaterialRenderInput(
                blend);

    RequireNear(
        packed.TotalWeight(),
        1.0,
        2.0e-5,
        "Renderer changed the canonical M21 blend normalization.");

    Require(
        packed.exposedBedrock ==
            terrain_geology::
                reference_rock::
                    Basalt,
        "Renderer lost exposed basalt identity.");
}

void TestDryBasaltDesertAddsDust()
{
    BiomeService service(
        Body());

    service.UpsertBiome(
        Desert());

    const auto weights =
        FullBiomeWeight(
            service,
            DesertId());

    const auto blend =
        ResolveSurfaceMaterialBlend(
            Surface(
                Cell(
                    0.0F,
                    0.0F,
                    0.05F)),
            service,
            weights,
            {
                .slopeDegrees = 8.0F,
                .curvature = 0.0F,
                .dustAvailability = 1.0F
            });

    Require(
        blend.Weight(
            RenderedSurfaceMaterialKind::
                Dust) >
            0.5F,
        "Dry basalt + desert did not produce dusty basalt.");

    Require(
        blend.Weight(
            RenderedSurfaceMaterialKind::
                Bedrock) >
            0.0F,
        "Dust layer incorrectly reclassified basalt.");
}

void TestDeepSandDesertStaysSand()
{
    BiomeService service(
        Body());

    service.UpsertBiome(
        Desert());

    const auto weights =
        FullBiomeWeight(
            service,
            DesertId());

    const auto blend =
        ResolveSurfaceMaterialBlend(
            Surface(
                Cell(
                    0.0F,
                    1.5F,
                    0.05F)),
            service,
            weights,
            {
                .slopeDegrees = 5.0F,
                .curvature = 0.0F,
                .dustAvailability = 1.0F
            });

    RequireNear(
        blend.Weight(
            RenderedSurfaceMaterialKind::
                Sand),
        1.0,
        1.0e-6,
        "Deep sand + desert did not remain physical sand.");

    RequireNear(
        blend.Weight(
            RenderedSurfaceMaterialKind::
                Dust),
        0.0,
        0.0,
        "Dust compatibility ignored exposed sand.");
}

void TestDeepSoilForestBuildsForestFloor()
{
    BiomeService service(
        Body());

    service.UpsertBiome(
        Forest());

    const auto weights =
        FullBiomeWeight(
            service,
            ForestId());

    const auto blend =
        ResolveSurfaceMaterialBlend(
            Surface(
                Cell(
                    1.0F,
                    0.0F,
                    0.75F)),
            service,
            weights,
            {
                .slopeDegrees = 5.0F,
                .curvature = -0.1F,
                .mossPotential = 0.8F,
                .litterAvailability = 1.0F
            });

    Require(
        blend.Weight(
            RenderedSurfaceMaterialKind::
                Litter) >
            0.5F,
        "Deep soil + forest did not produce a litter forest floor.");

    Require(
        blend.Weight(
            RenderedSurfaceMaterialKind::
                Soil) >
            0.0F,
        "Forest floor erased the real soil substrate without full coverage.");
}

void TestSnowIsExplicitPhysicalOverlay()
{
    BiomeService service(
        Body());

    BiomeDefinition alpine{
        .id = {
            .high = 0x4d3231414c50494eULL,
            .low = 1U
        },
        .name = "Alpine",
        .placement = {
            .minimumResolvedWeight = 0.0F,
            .enabled = true
        }
    };

    alpine.surface.layers = {
        {
            .kind =
                BiomeSurfaceLayerKind::
                    Snow,
            .strength = 1.0F,
            .compatibleExposed =
                BiomeExposedMaterialMask::All
        }
    };

    service.UpsertBiome(
        alpine);

    const auto weights =
        FullBiomeWeight(
            service,
            alpine.id);

    const auto blend =
        ResolveSurfaceMaterialBlend(
            Surface(
                Cell(
                    0.0F,
                    0.0F,
                    0.20F)),
            service,
            weights,
            {
                .snowCoverage = 1.0F
            });

    RequireNear(
        blend.Weight(
            RenderedSurfaceMaterialKind::
                Snow),
        1.0,
        1.0e-6,
        "Full snow process mask did not physically cover the surface.");

    RequireNear(
        blend.Weight(
            RenderedSurfaceMaterialKind::
                Bedrock),
        0.0,
        1.0e-6,
        "Bedrock remained visible through a full explicit snow overlay.");
}

void TestFeatureMasksAreSharedAndDeterministic()
{
    BiomeService service(
        Body());

    service.UpsertBiome(
        Forest());

    const auto weights =
        FullBiomeWeight(
            service,
            ForestId());

    const auto physical =
        Surface(
            Cell(
                0.0F,
                0.0F,
                0.85F));

    const SurfaceMaterialFeatureMasks steep{
        .slopeDegrees = 80.0F,
        .curvature = 0.0F,
        .mossPotential = 1.0F
    };

    const auto a =
        ResolveSurfaceMaterialBlend(
            physical,
            service,
            weights,
            steep);

    const auto b =
        ResolveSurfaceMaterialBlend(
            physical,
            service,
            weights,
            steep);

    RequireNear(
        a.Weight(
            RenderedSurfaceMaterialKind::
                Moss),
        b.Weight(
            RenderedSurfaceMaterialKind::
                Moss),
        0.0,
        "M21 fixed inputs are not deterministic.");

    Require(
        a.Weight(
            RenderedSurfaceMaterialKind::
                Moss) <
            0.1F,
        "Shared slope mask failed to suppress moss on a steep surface.");
}
} // namespace

int main()
{
    TestWetBasaltForestAddsMoss();
    TestDryBasaltDesertAddsDust();
    TestDeepSandDesertStaysSand();
    TestDeepSoilForestBuildsForestFloor();
    TestSnowIsExplicitPhysicalOverlay();
    TestFeatureMasksAreSharedAndDeterministic();

    std::cout
        << "Orbit M21 surface material resolver tests passed.\n";

    return EXIT_SUCCESS;
}
