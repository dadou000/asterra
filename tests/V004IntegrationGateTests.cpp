#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/selection/SelectionService.hpp>
#include <orbit/jobs/JobSystem.hpp>
#include <orbit/procedural_graph/ProceduralGraph.hpp>
#include <orbit/rhi/Resource.hpp>
#include <orbit/surface_authoring/TerrainConstraints.hpp>
#include <orbit/surface_model/SurfaceComposition.hpp>
#include <orbit/surface_model/SurfaceMaterialResolver.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain/GlobalTerrainFields.hpp>
#include <orbit/terrain_biome/BiomeService.hpp>
#include <orbit/terrain_debug/TerrainDebugPageData.hpp>
#include <orbit/terrain_debug/TerrainDebugRaster.hpp>
#include <orbit/terrain_dependency/TerrainDependencyGraph.hpp>
#include <orbit/terrain_erosion/StreamPowerErosion.hpp>
#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_gpu/PersistentGpuTerrainCache.hpp>
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
#include <any>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
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

class IntegrationBuffer final
    : public rhi::Buffer
{
public:
    explicit IntegrationBuffer(
        const u64 bytes)
        : storage_(
              static_cast<std::size_t>(
                  bytes))
    {
        if (bytes == 0U)
        {
            throw std::invalid_argument(
                "M31 integration buffer must be non-empty.");
        }
    }

    [[nodiscard]] u64 SizeBytes()
        const noexcept override
    {
        return static_cast<u64>(
            storage_.size());
    }

    [[nodiscard]] rhi::BufferUsage Usage()
        const noexcept override
    {
        return rhi::BufferUsage::Structured;
    }

    [[nodiscard]] rhi::MemoryUsage Memory()
        const noexcept override
    {
        return rhi::MemoryUsage::GpuOnly;
    }

    [[nodiscard]] std::byte* Map() override
    {
        return storage_.data();
    }

    void Unmap() override
    {
    }

private:
    std::vector<std::byte> storage_;
};

[[nodiscard]] std::shared_ptr<
    terrain_gpu::CachedGpuTerrainPage>
MakeIntegrationCachedPage(
    const u64 bytes,
    const terrain_gpu::CachedTerrainProductMask products)
{
    auto page =
        std::make_shared<
            terrain_gpu::CachedGpuTerrainPage>();

    page->products = products;
    page->buffers.push_back(
        std::make_shared<
            IntegrationBuffer>(
                bytes));

    return page;
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

        const auto* services =
            surfaces.ServicesForBody(
                *bodyId);

        const auto* geologyService =
            surfaces.GeologyForBody(
                *bodyId);

        const auto* processService =
            surfaces.ProcessesForBody(
                *bodyId);

        const auto* biomeService =
            surfaces.BiomesForBody(
                *bodyId);

        const auto* cacheService =
            surfaces.CacheForBody(
                *bodyId);

        Require(
            services != nullptr &&
            services->IsValid() &&
            geologyService != nullptr &&
            processService != nullptr &&
            processService->IsValid() &&
            biomeService != nullptr &&
            cacheService != nullptr &&
            cacheService->Stats().residentPages ==
                0U &&
            services->DefaultBedrock() ==
                terrain_geology::
                    reference_rock::Basalt,
            "M31 entry: rocky body did not automatically own valid geology/process/biome/cache services.");

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

    static_cast<void>(
        page.Deposit(
            0U,
            0U,
            LooseMaterialKind::Sand,
            0.75));

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


void TestPersistentGpuCacheReusesAcrossFrames()
{
    using namespace terrain_gpu;

    const terrain::PhysicalTerrainPageAddress
        address{
            .planet = {
                .high =
                    0x4D33314341434845ULL,
                .low =
                    0x0000000000000005ULL
            },
            .tile = {
                .face =
                    world::CubeFace::PositiveX,
                .level = 10U,
                .x = 511U,
                .y = 511U
            }
        };

    const PersistentGpuTerrainCacheKey key{
        .address = address,
        .physicalLod = 3U,
        .revisions = {
            .geology = 11U,
            .climate = 12U,
            .authoring = 13U,
            .biome = 14U,
            .water = 15U,
            .processes = 16U
        }
    };

    PersistentGpuTerrainCache cache({
        .maximumResidentBytes =
            8ULL * 1024ULL * 1024ULL,
        .maximumPages = 8U
    });

    u64 generationCount = 0U;
    std::shared_ptr<CachedGpuTerrainPage>
        first;

    constexpr u32 frameCount = 120U;

    for (u32 frame = 0U;
         frame < frameCount;
         ++frame)
    {
        auto page =
            cache.GetOrCreate(
                key,
                [&]()
                {
                    ++generationCount;

                    return
                        MakeIntegrationCachedPage(
                            4096U,
                            ProductBit(
                                CachedTerrainProduct::
                                    MaterialColumn) |
                            ProductBit(
                                CachedTerrainProduct::
                                    Drainage));
                });

        if (frame == 0U)
        {
            first = page;
        }

        Require(
            page != nullptr &&
            page == first,
            "M31 cache: stable physical page did not reuse the same resident solved payload across frames.");
    }

    const auto stats =
        cache.Stats();

    Require(
        generationCount == 1U &&
        stats.generations == 1U &&
        stats.misses == 1U &&
        stats.hits ==
            frameCount - 1U &&
        stats.insertions == 1U &&
        stats.evictions == 0U &&
        stats.residentPages == 1U &&
        stats.residentBytes == 4096U,
        "M31 cache: unchanged physical terrain regenerated or failed to stay resident across frames.");

    const auto fingerprint =
        PersistentGpuTerrainCacheFingerprint(
            key);

    Require(
        fingerprint != 0U &&
        cache.Find(key) == first &&
        cache.Stats().hits ==
            frameCount,
        "M31 cache: resident lookup after the frame sequence did not preserve page identity.");
}

void TestDependencyChangesRegenerateOnlyDependents()
{
    using namespace terrain_dependency;
    using namespace terrain_gpu;

    const world::PlanetId planet{
        .high =
            0x4D3331444550454EULL,
        .low =
            0x4400000000000006ULL
    };

    const terrain::PhysicalTerrainPageAddress
        center{
            .planet = planet,
            .tile = {
                .face =
                    world::CubeFace::PositiveX,
                .level = 9U,
                .x = 255U,
                .y = 255U
            }
        };

    const terrain::PhysicalTerrainPageAddress
        distant{
            .planet = planet,
            .tile =
                world::OffsetTile(
                    center.tile,
                    4,
                    0)
        };

    terrain::TerrainGenerationRevisions
        initial{
            .geology = 1U,
            .climate = 2U,
            .authoring = 3U,
            .biome = 4U,
            .water = 5U,
            .processes = 6U
        };

    PersistentGpuTerrainCache cache({
        .maximumResidentBytes =
            8ULL * 1024ULL * 1024ULL,
        .maximumPages = 8U
    });

    const auto cacheKeyFor =
        [&](const terrain::
                PhysicalTerrainPageAddress&
                    address,
            const terrain::
                TerrainGenerationRevisions&
                    revisions)
        {
            return
                PersistentGpuTerrainCacheKey{
                    .address = address,
                    .physicalLod = 2U,
                    .revisions = revisions
                };
        };

    cache.Insert(
        cacheKeyFor(
            center,
            initial),
        MakeIntegrationCachedPage(
            2048U,
            ProductBit(
                CachedTerrainProduct::
                    MaterialColumn)));

    cache.Insert(
        cacheKeyFor(
            distant,
            initial),
        MakeIntegrationCachedPage(
            2048U,
            ProductBit(
                CachedTerrainProduct::
                    MaterialColumn)));

    jobs::JobSystem jobs(
        2U);

    procedural_graph::ProceduralGraph
        graph(
            jobs);

    std::array<
        u64,
        kTerrainDependencyProductCount>
        centerBuilds{};

    std::array<
        u64,
        kTerrainDependencyProductCount>
        distantBuilds{};

    const auto productIndex =
        [](const TerrainDependencyProduct
               product)
        {
            return
                static_cast<std::size_t>(
                    product);
        };

    TerrainDependencyGraph dependencies(
        graph,
        [&](const terrain::
                PhysicalTerrainPageAddress&
                    address,
            const TerrainDependencyProduct
                product,
            const procedural_graph::
                BuildContext&)
            -> std::any
        {
            if (address == center)
            {
                ++centerBuilds[
                    productIndex(
                        product)];
            }
            else if (address == distant)
            {
                ++distantBuilds[
                    productIndex(
                        product)];
            }
            else
            {
                throw std::runtime_error(
                    "M31 dependency: unexpected page build.");
            }

            return std::any(
                static_cast<u32>(
                    product));
        },
        &cache);

    dependencies.RegisterPage(
        center,
        initial);

    dependencies.RegisterPage(
        distant,
        initial);

    Require(
        dependencies.BuildBlocking(
            center,
            TerrainDependencyProduct::
                SurfaceMaterial) &&
        dependencies.BuildBlocking(
            center,
            TerrainDependencyProduct::
                Scatter) &&
        dependencies.BuildBlocking(
            distant,
            TerrainDependencyProduct::
                SurfaceMaterial) &&
        dependencies.BuildBlocking(
            distant,
            TerrainDependencyProduct::
                Scatter),
        "M31 dependency: initial product chain did not build.");

    const auto centerBefore =
        centerBuilds;

    const auto distantBefore =
        distantBuilds;

    Require(
        dependencies.BuildBlocking(
            center,
            TerrainDependencyProduct::
                SurfaceMaterial) &&
        dependencies.BuildBlocking(
            center,
            TerrainDependencyProduct::
                Scatter),
        "M31 dependency: clean products could not be re-requested.");

    Require(
        centerBuilds ==
            centerBefore,
        "M31 dependency: unchanged terrain rebuilt without an authority change.");

    const auto surfaceChange =
        dependencies.ApplyChange({
            .kind =
                TerrainChangeKind::
                    BiomeSurfaceMaterial,
            .scope = {
                .planet = planet,
                .global = false,
                .center = center.tile,
                .radiusTiles = 0U,
                .downstreamRadiusTiles = 0U
            }
        });

    Require(
        surfaceChange.affectedPages ==
                1U &&
        surfaceChange.cacheEntriesRemoved ==
                1U &&
        surfaceChange.dirtyProducts ==
            ProductBit(
                TerrainDependencyProduct::
                    SurfaceMaterial),
        "M31 dependency: surface-only edit invalidated the wrong page/product set.");

    const auto surfaceStatus =
        dependencies.Status(
            center,
            TerrainDependencyProduct::
                SurfaceMaterial);

    const auto scatterStatus =
        dependencies.Status(
            center,
            TerrainDependencyProduct::
                Scatter);

    Require(
        surfaceStatus.has_value() &&
        scatterStatus.has_value() &&
        surfaceStatus->state ==
            procedural_graph::
                NodeState::Dirty &&
        scatterStatus->state ==
            procedural_graph::
                NodeState::Clean,
        "M31 dependency: surface-only edit dirtied unrelated scatter state.");

    Require(
        dependencies.BuildBlocking(
            center,
            TerrainDependencyProduct::
                SurfaceMaterial),
        "M31 dependency: dirty surface material did not rebuild.");

    for (std::size_t index = 0U;
         index <
             kTerrainDependencyProductCount;
         ++index)
    {
        const auto product =
            static_cast<
                TerrainDependencyProduct>(
                    index);

        const u64 expected =
            centerBefore[index] +
            (product ==
                 TerrainDependencyProduct::
                     SurfaceMaterial
                 ? 1U
                 : 0U);

        Require(
            centerBuilds[index] ==
                expected,
            "M31 dependency: surface edit rebuilt a non-dependent product.");
    }

    Require(
        distantBuilds ==
            distantBefore &&
        cache.Stats().residentPages ==
            1U,
        "M31 dependency: bounded edit regenerated or evicted out-of-scope terrain.");

    const auto revisionsAfterSurface =
        dependencies.Revisions(
            center);

    Require(
        revisionsAfterSurface.
                has_value() &&
        revisionsAfterSurface->
                biome ==
            initial.biome + 1U &&
        revisionsAfterSurface->
                geology ==
            initial.geology &&
        revisionsAfterSurface->
                processes ==
            initial.processes,
        "M31 dependency: biome surface edit advanced unrelated authority revisions.");

    const auto scatterBefore =
        centerBuilds[
            productIndex(
                TerrainDependencyProduct::
                    Scatter)];

    const auto surfaceBeforeScatter =
        centerBuilds[
            productIndex(
                TerrainDependencyProduct::
                    SurfaceMaterial)];

    const auto scatterChange =
        dependencies.ApplyChange({
            .kind =
                TerrainChangeKind::
                    BiomeScatter,
            .scope = {
                .planet = planet,
                .global = false,
                .center = center.tile,
                .radiusTiles = 0U,
                .downstreamRadiusTiles = 0U
            }
        });

    Require(
        scatterChange.affectedPages ==
                1U &&
        scatterChange.dirtyProducts ==
            ProductBit(
                TerrainDependencyProduct::
                    Scatter),
        "M31 dependency: scatter-only edit invalidated the wrong product set.");

    Require(
        dependencies.BuildBlocking(
            center,
            TerrainDependencyProduct::
                Scatter) &&
        centerBuilds[
            productIndex(
                TerrainDependencyProduct::
                    Scatter)] ==
            scatterBefore + 1U &&
        centerBuilds[
            productIndex(
                TerrainDependencyProduct::
                    SurfaceMaterial)] ==
            surfaceBeforeScatter,
        "M31 dependency: scatter edit did not regenerate exactly the scatter descendant.");

    Require(
        distantBuilds ==
            distantBefore,
        "M31 dependency: dependency-only regeneration crossed the bounded spatial scope.");
}

void TestAllDebugFieldsAreInspectable()
{
    using namespace terrain_debug;

    constexpr u32 width = 4U;
    constexpr u32 height = 4U;
    constexpr std::size_t texels =
        static_cast<std::size_t>(
            width) *
        height;

    const TerrainDebugPageStamp stamp{
        .address = {
            .planet = {
                .high =
                    0x4D33314445425547ULL,
                .low =
                    0x0000000000000007ULL
            },
            .tile = {
                .face =
                    world::CubeFace::PositiveY,
                .level = 8U,
                .x = 120U,
                .y = 121U
            }
        },
        .physicalLod = 4U,
        .revisions = {
            .geology = 11U,
            .climate = 12U,
            .authoring = 13U,
            .biome = 14U,
            .water = 15U,
            .processes = 16U
        },
        .cacheResident = true,
        .invalidationRevision = 77U
    };

    TerrainDebugPageData page(
        stamp,
        width,
        height);

    std::array<f32, texels>
        scalar{};

    std::array<
        TerrainDebugVector2,
        texels>
        vector{};

    std::array<u32, texels>
        category{};

    for (std::size_t index = 0U;
         index < texels;
         ++index)
    {
        scalar[index] =
            static_cast<f32>(
                index) *
            0.125F;

        vector[index] = {
            .x =
                static_cast<f32>(
                    index) *
                0.25F,
            .y =
                -static_cast<f32>(
                    index) *
                0.125F
        };

        category[index] =
            static_cast<u32>(
                index + 1U);
    }

    const auto catalog =
        FieldCatalog();

    Require(
        catalog.size() ==
            kRequiredTerrainDebugFieldCount &&
        catalog.size() == 21U,
        "M31 debug: required M29 debug-field catalog is incomplete.");

    for (const auto& descriptor :
         catalog)
    {
        Require(
            !descriptor.name.empty() &&
            !descriptor.upstream.empty(),
            "M31 debug: field is missing display name or upstream provenance.");

        for (const auto stage :
             descriptor.upstream)
        {
            const auto name =
                StageName(stage);

            Require(
                !name.empty() &&
                name != "Unknown",
                "M31 debug: field provenance contains an unknown stage.");
        }

        if (!page.Has(
                descriptor.field))
        {
            switch (
                descriptor.valueClass)
            {
            case TerrainDebugValueClass::
                Scalar:
            case TerrainDebugValueClass::
                SignedScalar:
                page.SetScalar(
                    descriptor.field,
                    scalar);
                break;

            case TerrainDebugValueClass::
                Vector:
                page.SetVector(
                    descriptor.field,
                    vector);
                break;

            case TerrainDebugValueClass::
                Category:
                page.SetCategory(
                    descriptor.field,
                    category);
                break;

            case TerrainDebugValueClass::
                Boolean:
            case TerrainDebugValueClass::
                Revision:
            case TerrainDebugValueClass::
                Lod:
                throw std::runtime_error(
                    "M31 debug: provenance field was not populated by the physical page stamp.");
            }
        }

        Require(
            page.Has(
                descriptor.field),
            "M31 debug: required field cannot be inspected on a physical page.");

        const auto view =
            page.View(
                descriptor.field);

        Require(
            TerrainDebugTexelCount(
                view) ==
                texels,
            "M31 debug: inspectable field has the wrong physical-page extent.");

        switch (
            descriptor.valueClass)
        {
        case TerrainDebugValueClass::
            Scalar:
        case TerrainDebugValueClass::
            SignedScalar:
            Require(
                view.scalar.size() ==
                    texels,
                "M31 debug: scalar field is not exposed through a typed raster view.");
            break;

        case TerrainDebugValueClass::
            Vector:
            Require(
                view.vector.size() ==
                    texels,
                "M31 debug: vector field is not exposed through a typed raster view.");
            break;

        case TerrainDebugValueClass::
            Category:
            Require(
                view.category.size() ==
                    texels,
                "M31 debug: category field is not exposed through a typed raster view.");
            break;

        case TerrainDebugValueClass::
            Boolean:
            Require(
                view.boolean.size() ==
                    texels,
                "M31 debug: cache residency field is not exposed through a typed raster view.");
            break;

        case TerrainDebugValueClass::
            Revision:
            Require(
                view.revision.size() ==
                        texels &&
                view.revision.front() ==
                    stamp.
                        invalidationRevision,
                "M31 debug: invalidation provenance is not inspectable.");
            break;

        case TerrainDebugValueClass::
            Lod:
            Require(
                view.lod.size() ==
                        texels &&
                view.lod.front() ==
                    stamp.physicalLod,
                "M31 debug: physical LOD provenance is not inspectable.");
            break;
        }

        const auto rgba =
            ComposeTerrainDebugRgba8(
                view);

        Require(
            rgba.size() ==
                texels * 4U,
            "M31 debug: inspectable field cannot be composed into the debug presentation raster.");
    }

    Require(
        DebugPageFingerprint(
            page.Stamp()) != 0U &&
        page.Stamp().cacheResident &&
        page.Stamp().
                invalidationRevision ==
            77U,
        "M31 debug: page-level cache/invalidation/LOD provenance is incomplete.");
}


void TestSaveLoadRegeneratesEquivalentTerrain()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-v004-m31-save-load-" +
         documents::ProjectId::Random().
             ToString());

    std::filesystem::remove_all(
        root);

    scene::ObjectId bodyObject{};
    scene::ObjectId terrainObject{};
    scene::ObjectId biomeObject{};
    scene::ObjectId maskObject{};
    scene::ObjectId surfaceLayerObject{};
    scene::ObjectId scatterObject{};

    documents::ProjectId projectId{};
    documents::WorldId worldId{};
    universe::BodyId bodyIdBefore{};

    terrain::AnalyticTerrainDesc
        recipeBefore{};

    std::array<
        terrain::TerrainSample,
        3U>
        samplesBefore{};

    terrain_biome::BiomeId
        biomeIdBefore{};

    terrain_biome::BiomePlacementEvaluation
        placementBefore{};

    std::vector<
        terrain_biome::ResolvedBiomeWeight>
        resolvedBefore;

    const std::array<
        math::Double3,
        3U>
        sampleDirections{{
            {1.0, 0.0, 0.0},
            {0.0, 1.0, 0.0},
            math::Normalize(
                math::Double3{
                    0.37,
                    -0.21,
                    0.90})
        }};

    terrain_biome::BiomePlacementContext
        placementContext{};

    placementContext.unitDirection =
        {1.0, 0.0, 0.0};
    placementContext.planetRadiusMeters =
        6'000'000.0;
    placementContext.temperatureC =
        31.0;
    placementContext.moisture =
        0.18;
    placementContext.rainfall =
        0.12;
    placementContext.elevationMeters =
        850.0;
    placementContext.slopeDegrees =
        9.0;
    placementContext.aspectRadians =
        0.35;
    placementContext.latitudeRadians =
        0.0;
    placementContext.continentality =
        0.70;
    placementContext.
        distanceToCoastWaterMeters =
        35'000.0;
    placementContext.drainage =
        0.20;
    placementContext.soilDepthMeters =
        0.35;
    placementContext.sandDepthMeters =
        0.10;
    placementContext.substrateRock =
        terrain_geology::
            reference_rock::Basalt;
    placementContext.solarExposure =
        0.80;
    placementContext.windExposure =
        0.55;
    placementContext.snowPersistence =
        0.0;

    Require(
        placementContext.IsValid(),
        "M31 save/load: biome placement probe is invalid.");

    auto project =
        documents::ProjectDocument::Create(
            root,
            "M31 Save Load Integration");

    projectId =
        project.Manifest().projectId;

    const auto manifestPath =
        project.ManifestPath();

    const auto worldPath =
        project.StartupWorldPath();

    try
    {
        {
            documents::WorldDatabase world(
                worldPath);

            worldId =
                world.Id();

            schema::SchemaRegistry schemas;
            world_model::RegisterSchemas(
                schemas);

            scene::ObjectStore objects(
                world);

            commands::CommandService commands(
                objects,
                schemas);

            selection::SelectionService
                selection;

            const auto worldObject =
                commands.CreateObject(
                    world_model::kWorldType,
                    "World");

            const auto systemObject =
                commands.CreateObject(
                    world_model::
                        kCelestialSystemType,
                    "Helion",
                    worldObject);

            bodyObject =
                commands.CreateObject(
                    world_model::
                        kCelestialBodyType,
                    "Asterra",
                    systemObject);

            terrainObject =
                commands.CreateObject(
                    world_model::
                        kTerrainSurfaceType,
                    "Asterra Terrain",
                    bodyObject);

            commands.SetProperty(
                bodyObject,
                world_model::kBodyRadius,
                6'000'000.0);

            commands.SetProperty(
                terrainObject,
                world_model::kTerrainSeed,
                i64{9'876'543});

            const scene::ObjectId selected[] = {
                bodyObject
            };

            selection.Set(
                selected);

            editor_model::
                SurfaceAuthoringModel
                authoring(
                    objects,
                    commands,
                    selection);

            editor_model::
                SurfaceReliefSettings
                relief{};

            relief.macroAmplitudeMeters =
                2'750.0;
            relief.macroWavelengthMeters =
                540'000.0;
            relief.detailAmplitudeMeters =
                390.0;
            relief.detailWavelengthMeters =
                19'000.0;
            relief.detailOctaves = 11;
            relief.maximumElevationMeters =
                9'500.0;

            authoring.SetRelief(
                terrainObject,
                relief);

            biomeObject =
                authoring.AddBiome(
                    terrainObject,
                    "M31 Dry Highlands");

            editor_model::
                SurfaceBiomePreferences
                preferences{};

            preferences.temperature = {
                20.0,
                42.0,
                6.0,
                8.0
            };

            preferences.moisture = {
                0.0,
                0.30,
                0.05,
                0.15
            };

            preferences.elevation = {
                250.0,
                2'600.0,
                400.0,
                650.0
            };

            authoring.SetCommonPreferences(
                biomeObject,
                preferences);

            maskObject =
                authoring.PaintLocalOverride(
                    biomeObject,
                    {1.0, 0.0, 0.0},
                    5'000.0,
                    25'000.0,
                    0.82,
                    0.65);

            surfaceLayerObject =
                authoring.AddSurfaceLayer(
                    biomeObject,
                    terrain_biome::
                        BiomeSurfaceLayerKind::
                            Dust);

            scatterObject =
                authoring.AddScatterRule(
                    biomeObject,
                    terrain_biome::
                        BiomeScatterKind::
                            Stone);

            auto settings =
                authoring.BiomeSettings(
                    biomeObject);

            settings.materialInfluence =
                0.72;
            settings.scatterDensityMultiplier =
                1.35;
            settings.hydraulicErosion =
                0.70;
            settings.aeolianTransport =
                1.40;

            authoring.SetBiomeSettings(
                biomeObject,
                settings);

            world_model::UniverseComposition
                universe;

            Require(
                universe.Rebuild(
                    objects).bodies ==
                    1U,
                "M31 save/load: pre-save universe did not compose one rocky body.");

            bodyIdBefore =
                *universe.BodyForObject(
                    bodyObject);

            surface_model::SurfaceComposition
                surfaces;

            const auto stats =
                surfaces.Rebuild(
                    objects,
                    universe);

            Require(
                stats.terrainSurfaces ==
                        1U &&
                stats.biomeServices ==
                        1U &&
                stats.biomeDefinitions ==
                        2U,
                "M31 save/load: pre-save surface composition is incomplete.");

            const auto* capability =
                surfaces.Registry().
                    FindTerrainSurface(
                        bodyIdBefore);

            const auto* analytic =
                capability != nullptr
                    ? dynamic_cast<
                          const terrain::
                              AnalyticTerrainSource*>(
                          capability->
                              terrain.get())
                    : nullptr;

            Require(
                analytic != nullptr,
                "M31 save/load: pre-save terrain source is missing.");

            recipeBefore =
                analytic->Description();

            for (std::size_t index = 0U;
                 index <
                     sampleDirections.size();
                 ++index)
            {
                samplesBefore[index] =
                    capability->
                        terrain->Sample({
                            .unitDirection =
                                sampleDirections[
                                    index],
                            .footprintMeters =
                                24.0,
                            .planet =
                                surfaces.
                                    Registry().
                                    SphericalPlanetDefinition(
                                        bodyIdBefore)->
                                    id,
                            .radialOffsetMeters =
                                0.0
                        });
            }

            const auto* biomes =
                surfaces.BiomesForBody(
                    bodyIdBefore);

            Require(
                biomes != nullptr &&
                biomes->Definitions().
                        size() ==
                    2U,
                "M31 save/load: optional biome did not reach the pre-save runtime service.");

            const auto foundBiome =
                std::find_if(
                    biomes->Definitions().
                        begin(),
                    biomes->Definitions().
                        end(),
                    [&](const auto& biome)
                    {
                        return
                            biome.id !=
                            biomes->
                                BaseBiome().id;
                    });

            Require(
                foundBiome !=
                    biomes->Definitions().
                        end(),
                "M31 save/load: optional runtime biome is missing.");

            biomeIdBefore =
                foundBiome->id;

            placementBefore =
                biomes->EvaluatePlacement(
                    *foundBiome,
                    placementContext);

            resolvedBefore =
                biomes->ResolvePlacement(
                    placementContext);

            auto* services =
                surfaces.ServicesForBody(
                    bodyIdBefore);

            Require(
                services != nullptr &&
                services->IsValid(),
                "M31 save/load: pre-save terrain service bundle is invalid.");

            const auto planet =
                surfaces.Registry().
                    SphericalPlanetDefinition(
                        bodyIdBefore);

            const terrain_gpu::
                PersistentGpuTerrainCacheKey
                cacheKey{
                    .address = {
                        .planet =
                            planet->id,
                        .tile =
                            world::
                                TileForDirection(
                                    sampleDirections[
                                        0U],
                                    8U)
                    },
                    .physicalLod = 2U,
                    .revisions = {
                        .geology = 1U,
                        .climate = 1U,
                        .authoring = 1U,
                        .biome = 1U,
                        .water = 1U,
                        .processes = 1U
                    }
                };

            services->Cache().Insert(
                cacheKey,
                MakeIntegrationCachedPage(
                    4096U,
                    terrain_gpu::
                        ProductBit(
                            terrain_gpu::
                                CachedTerrainProduct::
                                    MaterialColumn)));

            Require(
                services->Cache().
                        Stats().
                        residentPages ==
                    1U,
                "M31 save/load: pre-close derived cache fixture did not become resident.");

            world.Checkpoint();
        }

        auto reopenedProject =
            documents::
                ProjectDocument::Open(
                    manifestPath);

        Require(
            reopenedProject.
                    Manifest().
                    projectId ==
                projectId &&
            reopenedProject.
                    StartupWorldPath() ==
                worldPath,
            "M31 save/load: project identity/startup world changed after reopen.");

        {
            documents::WorldDatabase world(
                reopenedProject.
                    StartupWorldPath());

            Require(
                world.Id() ==
                    worldId,
                "M31 save/load: WorldId changed after reopen.");

            schema::SchemaRegistry schemas;
            world_model::RegisterSchemas(
                schemas);

            scene::ObjectStore objects(
                world);

            const std::array<
                scene::ObjectId,
                6U>
                persistedObjects{{
                    bodyObject,
                    terrainObject,
                    biomeObject,
                    maskObject,
                    surfaceLayerObject,
                    scatterObject
                }};

            for (const auto object :
                 persistedObjects)
            {
                Require(
                    objects.Find(object).
                        has_value(),
                    "M31 save/load: authored semantic ObjectId did not survive reopen.");
            }

            commands::CommandService commands(
                objects,
                schemas);

            selection::SelectionService
                selection;

            editor_model::
                SurfaceAuthoringModel
                authoring(
                    objects,
                    commands,
                    selection);

            const auto relief =
                authoring.Relief(
                    terrainObject);

            Require(
                relief.macroAmplitudeMeters ==
                        2'750.0 &&
                relief.macroWavelengthMeters ==
                        540'000.0 &&
                relief.detailAmplitudeMeters ==
                        390.0 &&
                relief.detailWavelengthMeters ==
                        19'000.0 &&
                relief.detailOctaves ==
                        11 &&
                relief.maximumElevationMeters ==
                        9'500.0,
                "M31 save/load: authored terrain relief did not survive reopen.");

            const auto masks =
                authoring.Masks(
                    biomeObject);

            Require(
                masks.size() ==
                        1U &&
                masks.front().id ==
                        maskObject &&
                masks.front().value ==
                        0.82 &&
                masks.front().opacity ==
                        0.65,
                "M31 save/load: authored biome override did not survive reopen.");

            world_model::UniverseComposition
                universe;

            static_cast<void>(
                universe.Rebuild(
                    objects));

            const auto bodyIdAfter =
                universe.BodyForObject(
                    bodyObject);

            Require(
                bodyIdAfter.has_value() &&
                *bodyIdAfter ==
                    bodyIdBefore,
                "M31 save/load: stable semantic body did not regenerate the same runtime BodyId.");

            surface_model::SurfaceComposition
                surfaces;

            static_cast<void>(
                surfaces.Rebuild(
                    objects,
                    universe));

            const auto* capability =
                surfaces.Registry().
                    FindTerrainSurface(
                        *bodyIdAfter);

            const auto* analytic =
                capability != nullptr
                    ? dynamic_cast<
                          const terrain::
                              AnalyticTerrainSource*>(
                          capability->
                              terrain.get())
                    : nullptr;

            Require(
                analytic != nullptr,
                "M31 save/load: reopened terrain source failed to regenerate.");

            const auto& recipeAfter =
                analytic->Description();

            Require(
                recipeAfter.seed ==
                        recipeBefore.seed &&
                recipeAfter.
                        macroAmplitudeMeters ==
                    recipeBefore.
                        macroAmplitudeMeters &&
                recipeAfter.
                        macroWavelengthMeters ==
                    recipeBefore.
                        macroWavelengthMeters &&
                recipeAfter.
                        detailAmplitudeMeters ==
                    recipeBefore.
                        detailAmplitudeMeters &&
                recipeAfter.
                        detailWavelengthMeters ==
                    recipeBefore.
                        detailWavelengthMeters &&
                recipeAfter.
                        detailOctaves ==
                    recipeBefore.
                        detailOctaves &&
                recipeAfter.
                        maximumElevationAboveSeaLevelMeters ==
                    recipeBefore.
                        maximumElevationAboveSeaLevelMeters,
                "M31 save/load: authored terrain recipe regenerated differently.");

            const auto planet =
                surfaces.Registry().
                    SphericalPlanetDefinition(
                        *bodyIdAfter);

            for (std::size_t index = 0U;
                 index <
                     sampleDirections.size();
                 ++index)
            {
                const auto after =
                    capability->
                        terrain->Sample({
                            .unitDirection =
                                sampleDirections[
                                    index],
                            .footprintMeters =
                                24.0,
                            .planet =
                                planet->id,
                            .radialOffsetMeters =
                                0.0
                        });

                const auto& before =
                    samplesBefore[index];

                Require(
                    after.elevationMeters ==
                            before.
                                elevationMeters &&
                    after.
                        coarseElevationMeters ==
                            before.
                                coarseElevationMeters &&
                    after.climate.
                            temperatureC ==
                        before.climate.
                            temperatureC &&
                    after.climate.
                            humidity ==
                        before.climate.
                            humidity &&
                    after.climate.
                            precipitation ==
                        before.climate.
                            precipitation &&
                    after.climate.
                            continentality ==
                        before.climate.
                            continentality &&
                    after.biomes.ocean ==
                        before.biomes.ocean &&
                    after.biomes.desert ==
                        before.biomes.desert &&
                    after.biomes.grassland ==
                        before.biomes.grassland &&
                    after.biomes.
                            temperateForest ==
                        before.biomes.
                            temperateForest &&
                    after.biomes.
                            borealForest ==
                        before.biomes.
                            borealForest &&
                    after.biomes.tundra ==
                        before.biomes.tundra &&
                    after.biomes.alpine ==
                        before.biomes.alpine &&
                    after.biomes.wetland ==
                        before.biomes.wetland &&
                    after.
                        standingWaterDepthMeters ==
                            before.
                                standingWaterDepthMeters,
                    "M31 save/load: regenerated derived terrain sample is not bit-identical.");
            }

            const auto* biomes =
                surfaces.BiomesForBody(
                    *bodyIdAfter);

            Require(
                biomes != nullptr,
                "M31 save/load: reopened biome service is missing.");

            const auto foundBiome =
                std::find_if(
                    biomes->Definitions().
                        begin(),
                    biomes->Definitions().
                        end(),
                    [&](const auto& biome)
                    {
                        return
                            biome.id ==
                            biomeIdBefore;
                    });

            Require(
                foundBiome !=
                    biomes->Definitions().
                        end(),
                "M31 save/load: runtime biome stable identity changed.");

            const auto placementAfter =
                biomes->EvaluatePlacement(
                    *foundBiome,
                    placementContext);

            Require(
                placementAfter.
                        automaticWeight ==
                    placementBefore.
                        automaticWeight &&
                placementAfter.
                        authoredWeight ==
                    placementBefore.
                        authoredWeight &&
                placementAfter.
                        finalWeight ==
                    placementBefore.
                        finalWeight,
                "M31 save/load: regenerated biome placement changed after reopen.");

            const auto resolvedAfter =
                biomes->ResolvePlacement(
                    placementContext);

            Require(
                resolvedAfter.size() ==
                    resolvedBefore.size(),
                "M31 save/load: resolved biome vector changed size.");

            for (std::size_t index = 0U;
                 index <
                     resolvedBefore.size();
                 ++index)
            {
                Require(
                    resolvedAfter[index].id ==
                            resolvedBefore[
                                index].id &&
                    resolvedAfter[index].
                            weight ==
                        resolvedBefore[
                            index].weight &&
                    resolvedAfter[index].
                            base ==
                        resolvedBefore[
                            index].base,
                    "M31 save/load: regenerated biome weights/IDs differ after reopen.");
            }

            const auto* services =
                surfaces.ServicesForBody(
                    *bodyIdAfter);

            Require(
                services != nullptr &&
                services->IsValid() &&
                services->Cache().
                        Stats().
                        residentPages ==
                    0U &&
                services->Cache().
                        Stats().
                        residentBytes ==
                    0U,
                "M31 save/load: derived cache persisted across application reopen instead of regenerating from authority.");

            world.Checkpoint();
        }
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

} // namespace

int main()
{
    try
    {
        TestDefaultRockyPlanetComposition();
        TestAuthoredCanyonDrivesDrainageAndErosion();
        TestExposureBurialDrivesRenderedMaterial();
        TestBiomeOverrideDrivesConstrainedScatter();
        TestPersistentGpuCacheReusesAcrossFrames();
        TestDependencyChangesRegenerateOnlyDependents();
        TestAllDebugFieldsAreInspectable();
        TestSaveLoadRegeneratesEquivalentTerrain();

        std::cout
            << "Orbit V0.0.4 M31 integration entry: "
            << "8 integration slices passed.\n";

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
