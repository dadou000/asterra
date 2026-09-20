#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/surface_authoring/TerrainConstraints.hpp>
#include <orbit/surface_model/SurfaceComposition.hpp>
#include <orbit/surface_model/SurfaceMaterialResolver.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain/GlobalTerrainFields.hpp>
#include <orbit/terrain_biome/BiomeService.hpp>
#include <orbit/terrain_erosion/StreamPowerErosion.hpp>
#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_hydrology/DrainagePage.hpp>
#include <orbit/terrain_macro_geology/MacroGeologyField.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>
#include <orbit/terrain_material_column/SurfaceResolver.hpp>
#include <orbit/terrain_render/SurfaceMaterial.hpp>
#include <orbit/terrain_scatter/DeterministicScatter.hpp>
#include <orbit/terrain_scatter/PhysicalSurface.hpp>
#include <orbit/world_model/UniverseComposition.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace
{
using namespace orbit;

void Require(
    const bool condition,
    const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

[[nodiscard]] bool NearlyEqual(
    const f64 a,
    const f64 b,
    const f64 epsilon = 1.0e-5)
{
    return std::abs(a - b) <= epsilon;
}

[[nodiscard]] terrain_geology::GeologicalMaterialLibrary
MakeIntegrationGeology()
{
    terrain_geology::GeologicalMaterialLibrary geology;

    geology.Upsert({
        .id =
            terrain_geology::
                reference_rock::Basalt,
        .name = "M31 basalt",
        .hardness = 0.82F,
        .cohesion = 0.78F,
        .hydraulicErodibility = 0.34F,
        .aeolianErodibility = 0.24F,
        .permeability = 0.18F,
        .chemicalWeatherability = 0.22F,
        .fractureTendency = 0.36F,
        .density = 2'900.0F
    });

    geology.Upsert({
        .id =
            terrain_geology::
                reference_rock::VolcanicAsh,
        .name = "M31 volcanic ash",
        .hardness = 0.18F,
        .cohesion = 0.16F,
        .hydraulicErodibility = 0.88F,
        .aeolianErodibility = 0.92F,
        .permeability = 0.52F,
        .chemicalWeatherability = 0.60F,
        .fractureTendency = 0.62F,
        .density = 1'650.0F
    });

    return geology;
}

void TestDefaultRockyPlanetComposition()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-v004-m31-entry-" +
         documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    try
    {
        auto project =
            documents::ProjectDocument::Create(
                root,
                "V0.0.4 M31 Integration Entry");

        documents::WorldDatabase world(
            project.StartupWorldPath());

        schema::SchemaRegistry schemas;
        world_model::RegisterSchemas(
            schemas);

        scene::ObjectStore objects(
            world);

        commands::CommandService commands(
            objects,
            schemas);

        const auto worldObject =
            commands.CreateObject(
                world_model::kWorldType,
                "World");

        const auto systemObject =
            commands.CreateObject(
                world_model::kCelestialSystemType,
                "Helion",
                worldObject);

        const auto bodyObject =
            commands.CreateObject(
                world_model::kCelestialBodyType,
                "Asterra",
                systemObject);

        const auto terrainObject =
            commands.CreateObject(
                world_model::kTerrainSurfaceType,
                "Asterra Terrain",
                bodyObject);

        // Deliberately set no body-radius, terrain recipe, biome, or process
        // properties. M31 must prove the ordinary default path is complete.
        world_model::UniverseComposition universe;

        const auto universeStats =
            universe.Rebuild(
                objects);

        Require(
            universeStats.bodies == 1U,
            "M31 entry: default semantic rocky body did not compose.");

        const auto bodyId =
            universe.BodyForObject(
                bodyObject);

        Require(
            bodyId.has_value(),
            "M31 entry: default rocky body has no stable runtime BodyId.");

        surface_model::SurfaceComposition
            surfaces;

        const auto surfaceStats =
            surfaces.Rebuild(
                objects,
                universe);

        Require(
            surfaceStats.terrainSurfaces == 1U &&
            surfaceStats.biomeServices == 1U &&
            surfaceStats.biomeDefinitions == 1U,
            "M31 entry: default rocky terrain must compose one terrain capability and one BaseBiome service.");

        Require(
            surfaces.BodyForTerrainObject(
                terrainObject) ==
                bodyId &&
            surfaces.TerrainObjectForBody(
                *bodyId) ==
                terrainObject,
            "M31 entry: semantic terrain/body mapping is not stable.");

        const auto* capability =
            surfaces.Registry().
                FindTerrainSurface(
                    *bodyId);

        Require(
            capability != nullptr &&
            capability->terrain != nullptr,
            "M31 entry: default rocky body is missing terrain capability.");

        const auto* analytic =
            dynamic_cast<
                const terrain::AnalyticTerrainSource*>(
                    capability->terrain.get());

        Require(
            analytic != nullptr,
            "M31 entry: default terrain capability is not the production analytic terrain source.");

        const terrain::AnalyticTerrainDesc defaults{};

        const auto& description =
            analytic->Description();

        Require(
            description.seed == defaults.seed &&
            description.macroAmplitudeMeters ==
                defaults.macroAmplitudeMeters &&
            description.macroWavelengthMeters ==
                defaults.macroWavelengthMeters &&
            description.detailAmplitudeMeters ==
                defaults.detailAmplitudeMeters &&
            description.detailWavelengthMeters ==
                defaults.detailWavelengthMeters &&
            description.detailOctaves ==
                defaults.detailOctaves &&
            description.maximumElevationAboveSeaLevelMeters ==
                defaults.maximumElevationAboveSeaLevelMeters,
            "M31 entry: no-input terrain surface did not resolve the production default recipe.");

        const auto* biomeService =
            surfaces.BiomesForBody(
                *bodyId);

        Require(
            biomeService != nullptr,
            "M31 entry: default rocky terrain has no BaseBiome service.");

        const auto definitions =
            biomeService->Definitions();

        const auto resolvedBase =
            biomeService->Resolve({});

        Require(
            definitions.size() == 1U &&
            definitions.front().id ==
                biomeService->BaseBiome().id &&
            resolvedBase.size() == 1U &&
            resolvedBase.front().base &&
            resolvedBase.front().weight == 1.0F,
            "M31 entry: BaseBiome is not the complete no-input fallback.");

        const auto planet =
            surfaces.Registry().
                SphericalPlanetDefinition(
                    *bodyId);

        Require(
            planet.has_value() &&
            planet->radiusMeters ==
                6'000'000.0,
            "M31 entry: default rocky body did not resolve the production spherical planet defaults.");

        constexpr std::array<
            math::Double3,
            4U>
            directions{{
                {1.0, 0.0, 0.0},
                {0.0, 1.0, 0.0},
                {0.0, 0.0, 1.0},
                {0.5773502691896258,
                 0.5773502691896258,
                 0.5773502691896258}
            }};

        for (const auto& direction :
             directions)
        {
            const auto sample =
                capability->terrain->Sample({
                    .unitDirection =
                        math::Normalize(
                            direction),
                    .footprintMeters =
                        32.0,
                    .planet =
                        planet->id,
                    .radialOffsetMeters =
                        0.0
                });

            const f32 biomeTotal =
                sample.biomes.ocean +
                sample.biomes.desert +
                sample.biomes.grassland +
                sample.biomes.temperateForest +
                sample.biomes.borealForest +
                sample.biomes.tundra +
                sample.biomes.alpine +
                sample.biomes.wetland;

            Require(
                std::isfinite(
                    sample.elevationMeters) &&
                std::isfinite(
                    sample.coarseElevationMeters) &&
                std::isfinite(
                    sample.climate.temperatureC) &&
                std::isfinite(
                    sample.climate.humidity) &&
                std::isfinite(
                    sample.climate.precipitation) &&
                std::isfinite(
                    sample.climate.continentality) &&
                std::isfinite(
                    sample.standingWaterDepthMeters) &&
                sample.standingWaterDepthMeters >=
                    0.0 &&
                std::abs(
                    static_cast<f64>(
                        biomeTotal) -
                    1.0) <=
                    1.0e-4,
                "M31 entry: default terrain sampling did not produce a finite complete surface sample.");
        }

        world.Checkpoint();
    }
    catch (...)
    {
        std::filesystem::remove_all(
            root);
        throw;
    }

    std::filesystem::remove_all(
        root);
}

void TestAuthoredCanyonDrivesDrainageAndErosion()
{
    using namespace surface_authoring;
    using namespace terrain_erosion;
    using namespace terrain_hydrology;
    using namespace terrain_macro_geology;
    using namespace terrain_material_column;

    constexpr u32 resolution = 7U;
    constexpr u32 centerY = resolution / 2U;

    auto geology =
        MakeIntegrationGeology();

    const world::PlanetDefinition planet{
        .radiusMeters = 6'000'000.0,
        .id = {
            .high =
                0x4D333143414E594FULL,
            .low =
                0x4E00000000000001ULL
        },
        .generationSeed =
            0x4D333143414E594FULL
    };

    terrain::PhysicalTerrainPageKey key{
        .address = {
            .planet = planet.id,
            .tile = {
                .face =
                    world::CubeFace::PositiveX,
                .level = 10U,
                .x = 511U,
                .y = 511U
            }
        },
        .resolution = resolution,
        .revisions = {
            .geology = 1U,
            .climate = 1U,
            .authoring = 1U,
            .biome = 1U,
            .water = 1U,
            .processes = 1U
        }
    };

    const auto bounds =
        world::TileBounds(
            key.address.tile);

    const auto pagePosition =
        [&](const u32 x,
            const u32 y)
        {
            const f64 tx =
                static_cast<f64>(x) /
                static_cast<f64>(
                    resolution - 1U);

            const f64 ty =
                static_cast<f64>(y) /
                static_cast<f64>(
                    resolution - 1U);

            return
                terrain::CanonicalizeSurfacePosition({
                    .planet = planet.id,
                    .unitDirection =
                        world::CubeToUnitDirection({
                            .face = bounds.face,
                            .uv = {
                                bounds.minimumUv.x +
                                    (bounds.maximumUv.x -
                                     bounds.minimumUv.x) *
                                        tx,
                                bounds.minimumUv.y +
                                    (bounds.maximumUv.y -
                                     bounds.minimumUv.y) *
                                        ty
                            }
                        }),
                    .radialOffsetMeters = 0.0
                });
        };

    const f64 spacingMeters =
        std::acos(
            std::clamp(
                math::Dot(
                    pagePosition(0U, centerY).
                        unitDirection,
                    pagePosition(1U, centerY).
                        unitDirection),
                -1.0,
                1.0)) *
        planet.radiusMeters;

    Require(
        std::isfinite(spacingMeters) &&
            spacingMeters > 100.0,
        "M31 canyon: physical page spacing is invalid.");

    MaterialColumnPage upstream(
        resolution,
        spacingMeters);

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const f32 centerDistance =
                static_cast<f32>(
                    std::abs(
                        static_cast<i32>(y) -
                        static_cast<i32>(centerY)));

            const f32 height =
                220.0F -
                static_cast<f32>(x) *
                    1.5F -
                centerDistance;

            upstream.SetCell(
                x,
                y,
                {
                    .bedrockHeightMeters =
                        height,
                    .referenceBedrockHeightMeters =
                        height,
                    .bedrockMaterial =
                        terrain_geology::
                            reference_rock::
                                VolcanicAsh
                });
        }
    }

    const SplineConstraintPrimitive canyon{
        .controlUnitDirections = {
            pagePosition(0U, centerY).
                unitDirection,
            pagePosition(
                resolution / 2U,
                centerY).
                unitDirection,
            pagePosition(
                resolution - 1U,
                centerY).
                unitDirection
        },
        .halfWidthMeters =
            spacingMeters * 0.30,
        .falloffMeters =
            spacingMeters * 0.35
    };

    TerrainConstraintSet authored{
        .id = {
            .high =
                0x4D333143414E594FULL,
            .low =
                0x4E00000000000100ULL
        },
        .planet = planet.id,
        .name =
            "M31 authored canyon"
    };

    authored.height.constraints.push_back({
        .id = {
            .high =
                0x4D333143414E594FULL,
            .low =
                0x4E00000000000101ULL
        },
        .mode =
            ConstraintCompositionMode::Add,
        .primitive = canyon,
        .value = -24.0,
        .opacity = 1.0,
        .enabled = true
    });

    authored.protection.constraints.push_back({
        .id = {
            .high =
                0x4D333143414E594FULL,
            .low =
                0x4E00000000000102ULL
        },
        .mode =
            ConstraintCompositionMode::Replace,
        .primitive = canyon,
        .value = 0.60,
        .opacity = 1.0,
        .enabled = true
    });

    authored.drainage.constraints.push_back({
        .id = {
            .high =
                0x4D333143414E594FULL,
            .low =
                0x4E00000000000103ULL
        },
        .mode =
            ConstraintCompositionMode::Replace,
        .primitive = canyon,
        .value = 1.0,
        .opacity = 1.0,
        .enabled = true
    });

    Require(
        authored.IsValid(),
        "M31 canyon: authored spline authority is invalid.");

    terrain::GlobalTerrainFieldDesc
        globalDesc{};
    globalDesc.seed =
        0x4D333143414E474CULL;

    terrain::GlobalTerrainFields globals(
        planet,
        globalDesc);

    MacroGeologyDesc macroDesc{};
    macroDesc.seed =
        0x4D333143414E4D41ULL;
    macroDesc.convergenceUpliftMeters =
        0.0;
    macroDesc.divergenceSubsidenceMeters =
        0.0;
    macroDesc.distortionAmplitude =
        0.0;
    macroDesc.distortionOctaves =
        1U;

    MacroGeologyField authoredMacro(
        planet,
        globals,
        &authored,
        macroDesc);

    MacroGeologyField controlMacro(
        planet,
        globals,
        nullptr,
        macroDesc);

    const auto authoredForcing =
        BuildStreamPowerForcing(
            upstream,
            key,
            planet,
            authoredMacro);

    const auto controlForcing =
        BuildStreamPowerForcing(
            upstream,
            key,
            planet,
            controlMacro);

    std::vector<DrainageCellInput>
        authoredDrainage(
            resolution * resolution);

    std::vector<DrainageCellInput>
        controlDrainage(
            resolution * resolution);

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const std::size_t index =
                static_cast<std::size_t>(y) *
                    resolution +
                x;

            authoredDrainage[index] = {
                .runoffMetersPerSecond =
                    0.001F,
                .authoredDrainage =
                    static_cast<f32>(
                        authoredMacro.
                            Sample(
                                pagePosition(
                                    x,
                                    y)).
                            drainageGuidance)
            };

            controlDrainage[index] = {
                .runoffMetersPerSecond =
                    0.001F,
                .authoredDrainage =
                    static_cast<f32>(
                        controlMacro.
                            Sample(
                                pagePosition(
                                    x,
                                    y)).
                            drainageGuidance)
            };
        }
    }

    const auto boundary =
        [](const f32 height)
        {
            return DrainageBoundaryCell{
                .surfaceHeightMeters =
                    height,
                .conditionedHeightMeters =
                    height
            };
        };

    DrainagePageHalo halo{};
    halo.revision =
        0x4D333143414E4841ULL;
    halo.north.assign(
        resolution,
        boundary(400.0F));
    halo.south.assign(
        resolution,
        boundary(400.0F));
    halo.west.assign(
        resolution,
        boundary(400.0F));
    halo.east.assign(
        resolution,
        boundary(100.0F));
    halo.corners = {
        boundary(400.0F),
        boundary(400.0F),
        boundary(400.0F),
        boundary(400.0F)
    };

    StreamPowerErosionConfig config{};
    config.iterations = 8U;
    config.upliftCouplingPerIteration =
        0.0;
    config.authoredHeightRelaxation =
        0.50;
    config.incisionCoefficientMetersPerIteration =
        2.0;
    config.referenceDrainageAreaSquareMeters =
        upstream.CellAreaSquareMeters();
    config.maximumIncisionMetersPerIteration =
        2.0;
    config.drainage.depressionPolicy =
        DepressionRoutingPolicy::
            FillToBoundary;
    config.drainage.minimumDrainageDropMeters =
        0.01F;
    config.drainage.authoredGuidanceWeight =
        0.75F;

    const auto authoredResult =
        SolveStreamPowerErosion(
            upstream,
            key,
            geology,
            authoredDrainage,
            halo,
            authoredForcing,
            config);

    auto controlKey = key;
    controlKey.revisions.authoring = 0U;

    const auto controlResult =
        SolveStreamPowerErosion(
            upstream,
            controlKey,
            geology,
            controlDrainage,
            halo,
            controlForcing,
            config);

    const auto& authoredCenter =
        authoredResult.At(
            resolution / 2U,
            centerY);

    const auto& controlCenter =
        controlResult.At(
            resolution / 2U,
            centerY);

    const auto& authoredDownstream =
        authoredResult.At(
            resolution - 2U,
            centerY);

    const auto& controlDownstream =
        controlResult.At(
            resolution - 2U,
            centerY);

    Require(
        authoredCenter.
                finalSurfaceHeightMeters <
            controlCenter.
                finalSurfaceHeightMeters -
                10.0F,
        "M31 canyon: authored spline did not remain a physical depression after erosion.");

    Require(
        authoredDownstream.
                finalDrainageAreaSquareMeters >
            controlDownstream.
                finalDrainageAreaSquareMeters +
                upstream.
                    CellAreaSquareMeters(),
        "M31 canyon: drainage did not react to the authored canyon.");

    Require(
        authoredCenter.
                cumulativeIncisionMeters >
            0.0F,
        "M31 canyon: erosion did not react to the authored drainage feature.");

    auto baked = upstream;

    const auto bake =
        ApplyStreamPowerErosionResult(
            baked,
            geology,
            authoredResult);

    Require(
        bake.totalIncisionMeters >
                0.0 &&
        baked.At(
            resolution / 2U,
            centerY).
                SurfaceHeightMeters() <
            upstream.At(
                resolution / 2U,
                centerY).
                SurfaceHeightMeters() -
                10.0F,
        "M31 canyon: solved erosion did not bake through the M10 to M08 physical path.");
}

void TestExposureBurialDrivesRenderedMaterial()
{
    using namespace surface_model;
    using namespace terrain_biome;
    using namespace terrain_material_column;

    auto geology =
        MakeIntegrationGeology();

    MaterialColumnPage page(
        1U,
        2.0);

    page.SetCell(
        0U,
        0U,
        {
            .bedrockHeightMeters = 100.0F,
            .referenceBedrockHeightMeters =
                100.0F,
            .bedrockMaterial =
                terrain_geology::
                    reference_rock::Basalt,
            .regolithMeters = 0.25F,
            .soilMeters = 0.50F,
            .sandMeters = 0.125F,
            .debrisMeters = 0.125F,
            .moisture = 0.80F
        });

    const universe::BodyId body{
        .high =
            0x4D33315355524641ULL,
        .low =
            0x4345000000000002ULL
    };

    const BiomeId biomeId{
        .high =
            0x4D33315355524641ULL,
        .low =
            0x4345000000000201ULL
    };

    BiomeDefinition biome{
        .id = biomeId,
        .name =
            "M31 exposed rock dressing",
        .placement = {
            .minimumResolvedWeight =
                0.0F,
            .enabled = true
        },
        .surface = {
            .materialInfluence =
                1.0F,
            .layers = {
                BiomeSurfaceLayerRule{
                    .kind =
                        BiomeSurfaceLayerKind::Moss,
                    .strength =
                        0.40F,
                    .compatibleExposed =
                        BiomeExposedMaterialMask::
                            Bedrock,
                    .minimumSlopeDegrees =
                        0.0F,
                    .maximumSlopeDegrees =
                        90.0F,
                    .minimumCurvature =
                        -1.0F,
                    .maximumCurvature =
                        1.0F,
                    .minimumMoisture =
                        0.0F,
                    .maximumMoisture =
                        1.0F
                }
            }
        }
    };

    BiomeService biomes(
        body);

    biomes.UpsertBiome(
        biome);

    const std::array<
        BiomeWeightContribution,
        1U>
        contributions{{
            {
                .id = biomeId,
                .weight = 1.0F
            }
        }};

    const auto weights =
        biomes.Resolve(
            contributions);

    const SurfaceMaterialFeatureMasks
        features{
            .slopeDegrees = 12.0F,
            .curvature = 0.0F,
            .mossPotential = 1.0F
        };

    const auto resolve =
        [&]()
        {
            const auto physical =
                ResolveSurface(
                    page.At(0U, 0U),
                    SampleColumnGeology(
                        page.At(0U, 0U),
                        geology));

            const auto blend =
                ResolveSurfaceMaterialBlend(
                    physical,
                    biomes,
                    weights,
                    features);

            return std::tuple{
                physical,
                blend,
                terrain_render::
                    MakeSurfaceMaterialRenderInput(
                        blend)
            };
        };

    const auto [
        covered,
        coveredBlend,
        coveredRender] =
        resolve();

    Require(
        covered.material ==
                ExposedSurfaceKind::Debris &&
        !coveredRender.
            exposedBedrock.IsValid() &&
        NearlyEqual(
            coveredBlend.Weight(
                RenderedSurfaceMaterialKind::
                    Debris),
            1.0),
        "M31 exposure: renderer did not follow actual loose physical cover.");

    const auto removed =
        page.Erode(
            0U,
            0U,
            1.0,
            geology);

    Require(
        NearlyEqual(
            removed.bedrockMeters,
            0.0,
            0.0),
        "M31 exposure: stripping one meter of loose cover shaved bedrock.");

    const auto [
        exposed,
        exposedBlend,
        exposedRender] =
        resolve();

    Require(
        exposed.BedrockExposed() &&
        exposed.exposedRock ==
            terrain_geology::
                reference_rock::Basalt &&
        exposedRender.exposedBedrock ==
            terrain_geology::
                reference_rock::Basalt &&
        NearlyEqual(
            exposedBlend.Weight(
                RenderedSurfaceMaterialKind::
                    Bedrock),
            0.60,
            1.0e-6) &&
        NearlyEqual(
            exposedBlend.Weight(
                RenderedSurfaceMaterialKind::
                    Moss),
            0.40,
            1.0e-6),
        "M31 exposure: stripping cover did not drive M18/M21/rendered basalt exposure.");

    const auto substrate =
        page.At(0U, 0U).
            bedrockMaterial;

    page.Deposit(
        0U,
        0U,
        LooseMaterialKind::Sand,
        0.75);

    const auto [
        buried,
        buriedBlend,
        buriedRender] =
        resolve();

    Require(
        page.At(0U, 0U).
                bedrockMaterial ==
            substrate &&
        buried.material ==
            ExposedSurfaceKind::Sand &&
        !buriedRender.
            exposedBedrock.IsValid() &&
        NearlyEqual(
            buriedBlend.Weight(
                RenderedSurfaceMaterialKind::
                    Sand),
            1.0) &&
        NearlyEqual(
            buriedBlend.Weight(
                RenderedSurfaceMaterialKind::
                    Bedrock),
            0.0),
        "M31 burial: deposition did not immediately replace rendered bedrock with actual exposed sand.");
}

void TestBiomeOverrideDrivesConstrainedScatter()
{
    using namespace terrain_biome;
    using namespace terrain_material_column;
    using namespace terrain_scatter;

    auto geology =
        MakeIntegrationGeology();

    const universe::BodyId body{
        .high =
            0x4D333142494F4D45ULL,
        .low =
            0x0000000000000003ULL
    };

    const BiomeId forestId{
        .high =
            0x4D333142494F4D45ULL,
        .low =
            0x0000000000000301ULL
    };

    const BiomeScatterLayerRule treeRule{
        .id = {
            .high =
                0x4D33315343415454ULL,
            .low =
                0x4552000000000301ULL
        },
        .kind =
            BiomeScatterKind::Tree,
        .densityPerSquareMeter =
            1.0F,
        .minimumSpacingMeters =
            4.0F,
        .seedSalt = 0x310U,
        .compatibleExposed =
            BiomeExposedMaterialMask::Soil,
        .requiresSoil = true,
        .minimumSoilDepthMeters =
            0.20F,
        .minimumSlopeDegrees =
            0.0F,
        .maximumSlopeDegrees =
            35.0F,
        .slopeFalloffDegrees =
            8.0F,
        .minimumMoisture =
            0.20F,
        .maximumMoisture =
            0.90F,
        .moistureFalloff =
            0.10F,
        .minimumScale =
            0.80F,
        .maximumScale =
            1.20F,
        .enabled = true
    };

    BiomeDefinition forest{
        .id = forestId,
        .name =
            "M31 authored forest",
        .placement = {
            .minimumResolvedWeight =
                0.05F,
            .enabled = true,
            .mode =
                BiomePlacementMode::
                    AutomaticAndAuthored,
            .selectors = {
                BiomeAutomaticSelector{
                    .field =
                        BiomeSelectorField::
                            Temperature,
                    .minimum = 10.0,
                    .maximum = 20.0,
                    .lowerFalloff = 5.0,
                    .upperFalloff = 5.0,
                    .enabled = true
                }
            },
            .authoredMasks = {
                BiomeAuthoredMask{
                    .id = {
                        .high =
                            0x4D33314D41534B31ULL,
                        .low =
                            0x0000000000000301ULL
                    },
                    .operation =
                        BiomeAuthoredWeightOperation::
                            Replace,
                    .centerUnitDirection =
                        {0.0, 1.0, 0.0},
                    .global = true,
                    .value = 0.75,
                    .opacity = 1.0,
                    .enabled = true
                }
            }
        },
        .scatter = {
            .densityMultiplier =
                1.0F,
            .layers = {
                treeRule
            }
        }
    };

    BiomeService biomes(
        body);

    biomes.UpsertBiome(
        forest);

    BiomePlacementContext warm{};
    warm.temperatureC = 15.0;

    BiomePlacementContext hot = warm;
    hot.temperatureC = 45.0;

    const auto warmEvaluation =
        biomes.EvaluatePlacement(
            forest,
            warm);

    const auto hotEvaluation =
        biomes.EvaluatePlacement(
            forest,
            hot);

    Require(
        NearlyEqual(
            warmEvaluation.automaticWeight,
            1.0) &&
        NearlyEqual(
            hotEvaluation.automaticWeight,
            0.0) &&
        NearlyEqual(
            warmEvaluation.finalWeight,
            0.75) &&
        NearlyEqual(
            hotEvaluation.finalWeight,
            0.75),
        "M31 biome: automatic climate response or authored override semantics are incorrect.");

    const auto resolved =
        biomes.ResolvePlacement(
            hot);

    f32 forestWeight = 0.0F;

    for (const auto& weight :
         resolved)
    {
        if (weight.id == forestId)
        {
            forestWeight =
                weight.weight;
        }
    }

    Require(
        NearlyEqual(
            forestWeight,
            0.75F,
            1.0e-6),
        "M31 biome: authored override did not survive resolved biome normalization.");

    constexpr u32 resolution = 16U;
    constexpr f32 spacingMeters = 4.0F;

    std::vector<ScatterCellInput>
        cells;

    cells.reserve(
        resolution * resolution);

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const bool soil =
                x <
                resolution / 2U;

            MaterialColumnCell column{
                .bedrockHeightMeters =
                    100.0F,
                .referenceBedrockHeightMeters =
                    100.0F,
                .bedrockMaterial =
                    terrain_geology::
                        reference_rock::Basalt,
                .soilMeters =
                    soil
                        ? 0.80F
                        : 0.0F,
                .moisture = 0.60F
            };

            const auto physical =
                ResolveSurface(
                    column,
                    SampleColumnGeology(
                        column,
                        geology));

            const auto scatterPhysical =
                MakePhysicalSurfaceScatterInput(
                    physical);

            cells.push_back({
                .biomeWeight =
                    forestWeight,
                .exposedMaterial =
                    scatterPhysical.material,
                .slopeDegrees =
                    10.0F,
                .soilDepthMeters =
                    column.soilMeters,
                .moisture =
                    scatterPhysical.moisture,
                .authoredDensity =
                    1.0F
            });
        }
    }

    const ScatterPageRequest request{
        .identity = {
            .planet = {
                .high =
                    0x4D33315343415454ULL,
                .low =
                    0x4552504C414E0003ULL
            },
            .tile = {
                .face =
                    world::CubeFace::PositiveX,
                .level = 12U,
                .x = 811U,
                .y = 438U
            },
            .sourceRevision = 1U,
            .scatterRevision = 1U,
            .generationSeed =
                0xA57E22A60031ULL
        },
        .gridResolution = resolution,
        .cellSizeMeters =
            spacingMeters,
        .rule = treeRule,
        .biomeDensityMultiplier =
            forest.scatter.
                densityMultiplier
    };

    const auto instances =
        GenerateDeterministicScatter(
            request,
            cells);

    Require(
        !instances.empty(),
        "M31 scatter: authored-resolved forest produced no constrained vegetation.");

    for (const auto& instance :
         instances)
    {
        Require(
            instance.kind ==
                    BiomeScatterKind::Tree &&
            instance.cellX <
                resolution / 2U,
            "M31 scatter: vegetation ignored the real exposed-soil constraint.");
    }
}

} // namespace

int main()
{
    try
    {
        TestDefaultRockyPlanetComposition();
        TestAuthoredCanyonDrivesDrainageAndErosion();
        TestExposureBurialDrivesRenderedMaterial();
        TestBiomeOverrideDrivesConstrainedScatter();

        std::cout
            << "Orbit V0.0.4 M31 integration entry: "
            << "4 integration slices passed.\n";

        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr
            << "Orbit V0.0.4 M31 integration entry failed: "
            << error.what()
            << '\n';

        return EXIT_FAILURE;
    }
}
