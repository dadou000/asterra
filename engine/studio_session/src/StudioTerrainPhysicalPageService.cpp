#include <orbit/studio_session/StudioTerrainPhysicalPageService.hpp>

#include <orbit/jobs/JobSystem.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/surface_authoring/TerrainConstraints.hpp>
#include <orbit/surface_model/SurfaceMaterialResolver.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain_biome/BiomeService.hpp>
#include <orbit/terrain_erosion/AeolianErosion.hpp>
#include <orbit/terrain_erosion/GlacialErosion.hpp>
#include <orbit/terrain_erosion/HydraulicErosion.hpp>
#include <orbit/terrain_erosion/RiverNetwork.hpp>
#include <orbit/terrain_erosion/SedimentExchange.hpp>
#include <orbit/terrain_erosion/StreamPowerErosion.hpp>
#include <orbit/terrain_erosion/ThermalErosion.hpp>
#include <orbit/terrain_hydrology/DrainagePage.hpp>
#include <orbit/terrain_macro_geology/MacroGeologyField.hpp>
#include <orbit/terrain_material_column/SurfaceResolver.hpp>
#include <orbit/terrain_scatter/DeterministicScatter.hpp>
#include <orbit/terrain_scatter/PhysicalSurface.hpp>
#include <orbit/terrain_water/CoastalProcess.hpp>
#include <orbit/world/PlanetTileNeighborhood.hpp>

#include <algorithm>
#include <any>
#include <array>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace orbit::studio_session
{
namespace
{
using Product =
    terrain_dependency::TerrainDependencyProduct;

struct AddressHash
{
    [[nodiscard]] std::size_t operator()(
        const terrain::PhysicalTerrainPageAddress& address) const noexcept
    {
        u64 hash =
            terrain::StableCombine64(
                0x4D31325041474553ULL,
                address.planet.high);
        hash =
            terrain::StableCombine64(
                hash,
                address.planet.low);
        hash =
            terrain::StableCombine64(
                hash,
                static_cast<u64>(
                    address.tile.face));
        hash =
            terrain::StableCombine64(
                hash,
                address.tile.level);
        hash =
            terrain::StableCombine64(
                hash,
                address.tile.x);
        hash =
            terrain::StableCombine64(
                hash,
                address.tile.y);
        return static_cast<std::size_t>(
            hash);
    }
};

[[nodiscard]] std::size_t CellIndex(
    const u32 resolution,
    const u32 x,
    const u32 y) noexcept
{
    return
        static_cast<std::size_t>(y) *
            resolution +
        x;
}

[[nodiscard]] f64 SampleCoordinate(
    const f64 minimum,
    const f64 maximum,
    const u32 coordinate,
    const u32 resolution) noexcept
{
    if (resolution <= 1U)
    {
        return
            0.5 *
            (minimum + maximum);
    }

    return
        minimum +
        (maximum - minimum) *
            static_cast<f64>(coordinate) /
            static_cast<f64>(
                resolution - 1U);
}

[[nodiscard]] terrain::PlanetSurfacePosition
PagePosition(
    const terrain::PhysicalTerrainPageAddress& address,
    const u32 resolution,
    const u32 x,
    const u32 y)
{
    const auto bounds =
        world::TileBounds(
            address.tile);

    const f64 u =
        SampleCoordinate(
            bounds.minimumUv.x,
            bounds.maximumUv.x,
            x,
            resolution);

    const f64 v =
        SampleCoordinate(
            bounds.minimumUv.y,
            bounds.maximumUv.y,
            y,
            resolution);

    return
        terrain::CanonicalizeSurfacePosition({
            .planet = address.planet,
            .unitDirection =
                world::CubeToUnitDirection({
                    .face = bounds.face,
                    .uv = {u, v}
                }),
            .radialOffsetMeters = 0.0
        });
}

[[nodiscard]] std::pair<u32, u32>
EdgeCoordinate(
    const world::TileEdge edge,
    const u32 sample,
    const u32 resolution)
{
    switch (edge)
    {
    case world::TileEdge::North:
        return {sample, 0U};
    case world::TileEdge::East:
        return {resolution - 1U, sample};
    case world::TileEdge::South:
        return {sample, resolution - 1U};
    case world::TileEdge::West:
        return {0U, sample};
    }

    return {0U, 0U};
}

[[nodiscard]] bool AddressMatches(
    const terrain::PhysicalTerrainPageAddress& address,
    const terrain_dependency::TerrainSpatialInvalidationScope& scope)
{
    if (address.planet != scope.planet)
    {
        return false;
    }

    if (scope.global)
    {
        return true;
    }

    if (address.tile.level !=
        scope.center.level)
    {
        return false;
    }

    const auto neighborhood =
        world::TileNeighborhood(
            scope.center,
            scope.radiusTiles +
                scope.downstreamRadiusTiles);

    return std::find(
               neighborhood.begin(),
               neighborhood.end(),
               address.tile) !=
        neighborhood.end();
}

void IncrementRevision(
    terrain::TerrainGenerationRevisions& revisions,
    const terrain_dependency::TerrainChangeKind kind) noexcept
{
    using Kind =
        terrain_dependency::TerrainChangeKind;

    switch (kind)
    {
    case Kind::RockPhysics:
        ++revisions.geology;
        break;
    case Kind::TerrainAuthoring:
        ++revisions.authoring;
        break;
    case Kind::Climate:
        ++revisions.climate;
        break;
    case Kind::Water:
        ++revisions.water;
        break;
    case Kind::ProcessSettings:
        ++revisions.processes;
        break;
    case Kind::BiomePlacement:
    case Kind::BiomeSurfaceMaterial:
    case Kind::BiomeScatter:
        ++revisions.biome;
        break;
    }
}

struct BodyBuildInputs
{
    world::PlanetDefinition planet{};
    std::shared_ptr<
        const terrain::AnalyticTerrainSource>
        source;

    terrain_geology::GeologicalMaterialLibrary geology;
    terrain_geology::RockTypeId defaultBedrock{};

    surface_model::TerrainProcessService processes{};
    terrain_biome::BiomeService biomes;
    surface_authoring::TerrainConstraintSet constraints{};

    f64 seaLevelMeters{0.0};
    u64 surfaceSourceRevision{0U};

    explicit BodyBuildInputs(
        const universe::BodyId body)
        : biomes(body)
    {
    }
};

struct SharedBodyInputs
{
    [[nodiscard]] std::shared_ptr<
        const BodyBuildInputs>
    Capture() const
    {
        std::scoped_lock lock(mutex);
        return current;
    }

    void Store(
        std::shared_ptr<
            const BodyBuildInputs> value)
    {
        std::scoped_lock lock(mutex);
        current =
            std::move(value);
        pending.reset();
    }

    void Stage(
        std::shared_ptr<
            const BodyBuildInputs> value)
    {
        std::scoped_lock lock(mutex);
        pending =
            std::move(value);
    }

    void CommitPending() noexcept
    {
        std::scoped_lock lock(mutex);

        if (pending != nullptr)
        {
            current =
                std::move(pending);
        }
    }

    mutable std::mutex mutex;
    std::shared_ptr<
        const BodyBuildInputs> current;
    std::shared_ptr<
        const BodyBuildInputs> pending;
};

struct PhysicalBundle
{
    terrain::PhysicalTerrainPageKey key{};
    u8 physicalLod{0U};
    f64 spacingMeters{0.0};

    std::vector<
        terrain::PlanetSurfacePosition>
        positions;
    std::vector<
        terrain::TerrainSample>
        sourceSamples;
    std::vector<
        terrain_macro_geology::MacroGeologySample>
        macroGeology;

    std::shared_ptr<
        const terrain_material_column::MaterialColumnPage>
        material;
    std::shared_ptr<
        const terrain_hydrology::DrainagePage>
        drainage;

    std::vector<
        terrain_hydrology::DrainageCellInput>
        drainageInputs;
    terrain_hydrology::DrainagePageHalo
        drainageHalo{};

    std::shared_ptr<
        const terrain_erosion::HydraulicErosionResult>
        hydraulic;
    std::shared_ptr<
        const terrain_erosion::AeolianErosionResult>
        aeolian;
    std::shared_ptr<
        const terrain_erosion::SedimentExchangePage>
        sedimentExchange;

    std::vector<
        terrain_erosion::AeolianCellForcing>
        aeolianForcing;

    std::vector<
        terrain_material_column::ExposedSurfaceState>
        exposedSurface;

    std::vector<
        std::vector<
            terrain_biome::ResolvedBiomeWeight>>
        biomeWeights;
    std::vector<f32>
        dominantBiomeWeights;
    std::vector<
        terrain_biome::BiomeId>
        dominantBiomes;

    std::vector<
        surface_model::ResolvedSurfaceMaterialBlend>
        surfaceMaterials;

    std::vector<f32>
        scatterDensityPerSquareMeter;
};

using BundlePtr =
    std::shared_ptr<const PhysicalBundle>;

[[nodiscard]] BundlePtr DependencyBundle(
    const procedural_graph::BuildContext& context,
    const std::size_t index)
{
    const auto* value =
        context.DependencyProduct<BundlePtr>(
            index);

    if (value == nullptr ||
        *value == nullptr)
    {
        throw std::logic_error(
            "M12 physical terrain build is missing a committed upstream bundle.");
    }

    return *value;
}

[[nodiscard]] terrain_geology::RockTypeId
ResolvedBedrock(
    const BodyBuildInputs& inputs,
    const terrain::PlanetSurfacePosition& position)
{
    const auto authored =
        surface_authoring::
            EvaluateTerrainConstraintSet(
                inputs.constraints,
                inputs.planet,
                position,
                {
                    .material =
                        inputs.defaultBedrock
                });

    if (authored.material.count == 0U)
    {
        return inputs.defaultBedrock;
    }

    const auto result =
        authored.material.materials[0];

    return result.IsValid()
        ? result
        : inputs.defaultBedrock;
}

[[nodiscard]] terrain_hydrology::DrainageBoundaryCell
BoundarySample(
    const BodyBuildInputs& inputs,
    const terrain_macro_geology::MacroGeologyField& macro,
    const terrain::PlanetSurfacePosition& position,
    const f64 footprintMeters)
{
    const auto sample =
        inputs.source->Sample({
            .unitDirection =
                position.unitDirection,
            .footprintMeters =
                footprintMeters,
            .planet =
                inputs.planet.id
        });

    const auto macroSample =
        macro.Sample(position);

    return {
        .surfaceHeightMeters =
            static_cast<f32>(
                sample.elevationMeters),
        .conditionedHeightMeters =
            static_cast<f32>(
                sample.elevationMeters),
        .authoredDrainage =
            static_cast<f32>(
                macroSample.
                    drainageGuidance),
        .drainageAreaSquareMeters = 0.0,
        .dischargeCubicMetersPerSecond = 0.0,
        .flowDx = 0,
        .flowDy = 0
    };
}

[[nodiscard]] terrain_hydrology::DrainagePageHalo
BuildDrainageHalo(
    const BodyBuildInputs& inputs,
    const terrain_macro_geology::MacroGeologyField& macro,
    const terrain::PhysicalTerrainPageAddress& address,
    const u32 resolution,
    const f64 spacingMeters,
    const u64 revision)
{
    terrain_hydrology::DrainagePageHalo halo{};
    halo.revision = revision;

    std::array<
            std::vector<
                terrain_hydrology::DrainageBoundaryCell>*,
            4U> sides{
                &halo.north,
                &halo.east,
                &halo.south,
                &halo.west
            };

    constexpr std::array<world::TileEdge, 4U>
        edges{
            world::TileEdge::North,
            world::TileEdge::East,
            world::TileEdge::South,
            world::TileEdge::West
        };

    for (u32 edgeIndex = 0U;
         edgeIndex < edges.size();
         ++edgeIndex)
    {
        auto& side =
            *sides[edgeIndex];

        side.resize(
            resolution);

        const auto mapping =
            world::NeighborAcrossTileEdge(
                address.tile,
                edges[edgeIndex]);

        for (u32 sampleIndex = 0U;
             sampleIndex < resolution;
             ++sampleIndex)
        {
            const u32 mapped =
                world::RemapTileEdgeSampleIndex(
                    mapping,
                    sampleIndex,
                    resolution);

            const auto [x, y] =
                EdgeCoordinate(
                    mapping.edge,
                    mapped,
                    resolution);

            side[sampleIndex] =
                BoundarySample(
                    inputs,
                    macro,
                    PagePosition(
                        {
                            .planet =
                                address.planet,
                            .tile =
                                mapping.tile
                        },
                        resolution,
                        x,
                        y),
                    spacingMeters);
        }
    }

    const auto bounds =
        world::TileBounds(
            address.tile);

    const std::array<math::Double2, 4U>
        cornerUv{
            math::Double2{
                bounds.minimumUv.x,
                bounds.minimumUv.y},
            math::Double2{
                bounds.maximumUv.x,
                bounds.minimumUv.y},
            math::Double2{
                bounds.maximumUv.x,
                bounds.maximumUv.y},
            math::Double2{
                bounds.minimumUv.x,
                bounds.maximumUv.y}
        };

    for (u32 index = 0U;
         index < 4U;
         ++index)
    {
        halo.corners[index] =
            BoundarySample(
                inputs,
                macro,
                terrain::CanonicalizeSurfacePosition({
                    .planet =
                        address.planet,
                    .unitDirection =
                        world::CubeToUnitDirection({
                            .face =
                                bounds.face,
                            .uv =
                                cornerUv[index]
                        })
                }),
                spacingMeters);
    }

    return halo;
}

[[nodiscard]] BundlePtr BuildGeology(
    const BodyBuildInputs& inputs,
    const terrain::PhysicalTerrainPageAddress& address,
    const u32 resolution,
    const terrain::TerrainGenerationRevisions& revisions,
    const procedural_graph::BuildContext&)
{
    auto result =
        std::make_shared<PhysicalBundle>();

    result->key.address =
        address;
    result->key.resolution =
        resolution;
    result->key.revisions =
        revisions;
    result->physicalLod =
        address.tile.level;

    result->spacingMeters =
        world::ApproximateTileWidthMeters(
            inputs.planet,
            address.tile) /
        static_cast<f64>(
            std::max(
                resolution - 1U,
                1U));

    const std::size_t count =
        static_cast<std::size_t>(
            resolution) *
        resolution;

    result->positions.resize(count);
    result->sourceSamples.resize(count);
    result->macroGeology.resize(count);

    auto material =
        std::make_shared<
            terrain_material_column::MaterialColumnPage>(
                resolution,
                result->spacingMeters);

    terrain_macro_geology::MacroGeologyField
        macro(
            inputs.planet,
            inputs.source->GlobalFields(),
            &inputs.constraints);

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const std::size_t index =
                CellIndex(
                    resolution,
                    x,
                    y);

            const auto position =
                PagePosition(
                    address,
                    resolution,
                    x,
                    y);

            const auto sample =
                inputs.source->Sample({
                    .unitDirection =
                        position.
                            unitDirection,
                    .footprintMeters =
                        result->
                            spacingMeters,
                    .planet =
                        inputs.planet.id
                });

            result->positions[index] =
                position;
            result->sourceSamples[index] =
                sample;
            result->macroGeology[index] =
                macro.Sample(
                    position);

            material->SetCell(
                x,
                y,
                {
                    .bedrockHeightMeters =
                        static_cast<f32>(
                            sample.
                                elevationMeters),
                    .referenceBedrockHeightMeters =
                        static_cast<f32>(
                            sample.
                                elevationMeters),
                    .bedrockMaterial =
                        ResolvedBedrock(
                            inputs,
                            position),
                    .regolithMeters = 0.0F,
                    .soilMeters = 0.0F,
                    .sandMeters = 0.0F,
                    .debrisMeters = 0.0F,
                    .moisture =
                        std::clamp(
                            sample.climate.
                                humidity,
                            0.0F,
                            1.0F),
                    .temporaryScalar = 0.0F
                });
        }
    }

    result->material =
        std::move(material);

    return result;
}

[[nodiscard]] BundlePtr BuildDrainage(
    const BodyBuildInputs& inputs,
    const terrain::TerrainGenerationRevisions& revisions,
    const procedural_graph::BuildContext& context)
{
    const BundlePtr upstream =
        DependencyBundle(
            context,
            0U);

    auto result =
        std::make_shared<PhysicalBundle>(
            *upstream);

    result->key.revisions =
        revisions;

    const u32 resolution =
        result->key.resolution;
    const std::size_t count =
        static_cast<std::size_t>(
            resolution) *
        resolution;

    result->drainageInputs.resize(
        count);

    for (std::size_t index = 0U;
         index < count;
         ++index)
    {
        result->drainageInputs[index] = {
            .runoffMetersPerSecond =
                static_cast<f32>(
                    std::max(
                        0.0,
                        inputs.processes.
                            hydraulic.
                            rainfallMetersPerSecond *
                        static_cast<f64>(
                            std::clamp(
                                result->
                                    sourceSamples[index].
                                    climate.
                                    precipitation,
                                0.0F,
                                1.0F)))),
            .authoredDrainage =
                static_cast<f32>(
                    result->
                        macroGeology[index].
                        drainageGuidance),
            .outlet = false
        };
    }

    terrain_macro_geology::MacroGeologyField
        macro(
            inputs.planet,
            inputs.source->GlobalFields(),
            &inputs.constraints);

    result->drainageHalo =
        BuildDrainageHalo(
            inputs,
            macro,
            result->key.address,
            resolution,
            result->spacingMeters,
            context.inputRevisionHash);

    result->drainage =
        std::make_shared<
            terrain_hydrology::DrainagePage>(
                terrain_hydrology::
                    BuildDrainagePage(
                        *result->material,
                        result->key,
                        result->
                            drainageInputs,
                        result->
                            drainageHalo,
                        inputs.processes.
                            streamPower.
                            drainage));

    return result;
}

void MergeSediment(
    terrain_erosion::SedimentExchangePage& destination,
    const terrain_erosion::SedimentExchangePage& source)
{
    const u32 resolution =
        destination.Resolution();

    if (source.Resolution() != resolution)
    {
        throw std::invalid_argument(
            "M12 cannot merge sediment pages with different resolution.");
    }

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const auto& cell =
                source.At(
                    x,
                    y);

            destination.Add(
                x,
                y,
                terrain_erosion::
                    SedimentTransportMedium::
                        Waterborne,
                cell.waterborne);
            destination.Add(
                x,
                y,
                terrain_erosion::
                    SedimentTransportMedium::
                        Airborne,
                cell.airborne);
            destination.Add(
                x,
                y,
                terrain_erosion::
                    SedimentTransportMedium::
                        SurfaceMobile,
                cell.surfaceMobile);
        }
    }
}

[[nodiscard]] BundlePtr BuildProcesses(
    const BodyBuildInputs& inputs,
    const terrain::TerrainGenerationRevisions& revisions,
    const procedural_graph::BuildContext& context)
{
    // TerrainProcesses depends on Geology then Drainage; the drainage bundle
    // already carries the exact geology product used to solve it.
    const BundlePtr upstream =
        DependencyBundle(
            context,
            1U);

    auto result =
        std::make_shared<PhysicalBundle>(
            *upstream);

    result->key.revisions =
        revisions;

    auto material =
        std::make_shared<
            terrain_material_column::MaterialColumnPage>(
                *result->material);

    terrain_macro_geology::MacroGeologyField
        macro(
            inputs.planet,
            inputs.source->GlobalFields(),
            &inputs.constraints);

    if (inputs.processes.
            streamPowerEnabled)
    {
        const auto forcing =
            terrain_erosion::
                BuildStreamPowerForcing(
                    *material,
                    result->key,
                    inputs.planet,
                    macro);

        const auto solved =
            terrain_erosion::
                SolveStreamPowerErosion(
                    *material,
                    result->key,
                    inputs.geology,
                    result->
                        drainageInputs,
                    result->
                        drainageHalo,
                    forcing,
                    inputs.processes.
                        streamPower);

        static_cast<void>(
            terrain_erosion::
                ApplyStreamPowerErosionResult(
                    *material,
                    inputs.geology,
                    solved));
    }

    std::optional<
        terrain_erosion::SedimentExchangePage>
        sediment;

    if (inputs.processes.
            hydraulicEnabled)
    {
        auto hydraulic =
            std::make_shared<
                terrain_erosion::
                    HydraulicErosionResult>(
                        terrain_erosion::
                            SimulateHydraulicErosion(
                                *material,
                                inputs.geology,
                                {},
                                inputs.processes.
                                    hydraulic));

        material =
            std::make_shared<
                terrain_material_column::
                    MaterialColumnPage>(
                        hydraulic->
                            material);

        if (hydraulic->
                sedimentExchange.
                has_value())
        {
            sediment =
                *hydraulic->
                    sedimentExchange;
        }

        result->hydraulic =
            std::move(
                hydraulic);
    }

    if (inputs.processes.
            thermalEnabled)
    {
        std::vector<f32> protection(
            result->
                macroGeology.size());

        for (std::size_t index = 0U;
             index <
                protection.size();
             ++index)
        {
            protection[index] =
                static_cast<f32>(
                    std::clamp(
                        result->
                            macroGeology[index].
                            protection,
                        0.0,
                        1.0));
        }

        const auto thermal =
            terrain_erosion::
                SimulateThermalErosion(
                    *material,
                    inputs.geology,
                    protection,
                    inputs.processes.
                        thermal);

        material =
            std::make_shared<
                terrain_material_column::
                    MaterialColumnPage>(
                        thermal.material);
    }

    if (inputs.processes.
            aeolianEnabled)
    {
        result->aeolianForcing.assign(
            result->macroGeology.size(),
            {});

        for (std::size_t index = 0U;
             index <
                result->
                    aeolianForcing.size();
             ++index)
        {
            // V0.0.4 has no independent wind-authority service yet. A missing
            // forcing source is represented physically as calm air, never as a
            // fabricated prevailing-wind algorithm.
            result->
                aeolianForcing[index].
                surfaceResistance =
                    static_cast<f32>(
                        std::clamp(
                            result->
                                macroGeology[index].
                                protection,
                            0.0,
                            1.0));
        }

        auto aeolian =
            std::make_shared<
                terrain_erosion::
                    AeolianErosionResult>(
                        terrain_erosion::
                            SimulateAeolianErosion(
                                *material,
                                inputs.geology,
                                result->
                                    aeolianForcing,
                                inputs.processes.
                                    aeolian,
                                sediment));

        material =
            std::make_shared<
                terrain_material_column::
                    MaterialColumnPage>(
                        aeolian->
                            material);

        if (aeolian->
                sedimentExchange.
                has_value())
        {
            sediment =
                *aeolian->
                    sedimentExchange;
        }

        result->aeolian =
            std::move(
                aeolian);
    }

    if (inputs.processes.
            glacialEnabled)
    {
        std::vector<
            terrain_erosion::
                GlacialClimateCell>
            climate(
                result->
                    sourceSamples.size());

        for (std::size_t index = 0U;
             index <
                climate.size();
             ++index)
        {
            const auto& sample =
                result->
                    sourceSamples[index];

            climate[index] = {
                .meanAnnualTemperatureC =
                    sample.climate.
                        temperatureC,
                .snowfallMetersIceEquivalentPerYear =
                    std::max(
                        sample.climate.
                            precipitation,
                        0.0F),
                .initialIceThicknessMeters =
                    0.0F,
                .processMask = 1.0F,
                .protection =
                    static_cast<f32>(
                        std::clamp(
                            result->
                                macroGeology[index].
                                protection,
                            0.0,
                            1.0))
            };
        }

        const auto glacial =
            terrain_erosion::
                SimulateGlacialErosion(
                    *material,
                    inputs.geology,
                    climate,
                    inputs.processes.
                        glacial);

        material =
            std::make_shared<
                terrain_material_column::
                    MaterialColumnPage>(
                        glacial.material);

        if (glacial.
                sedimentExchange.
                has_value())
        {
            if (!sediment.has_value())
            {
                sediment =
                    terrain_erosion::
                        SedimentExchangePage(
                            result->key.
                                resolution,
                            result->
                                spacingMeters);
            }

            MergeSediment(
                *sediment,
                *glacial.
                    sedimentExchange);
        }
    }

    if (inputs.processes.
            riversEnabled &&
        result->drainage != nullptr)
    {
        if (!sediment.has_value())
        {
            sediment =
                terrain_erosion::
                    SedimentExchangePage(
                        result->key.
                            resolution,
                        result->
                            spacingMeters);
        }

        const auto rivers =
            terrain_erosion::
                BuildRiverNetwork(
                    *result->drainage,
                    {},
                    inputs.processes.
                        rivers);

        static_cast<void>(
            terrain_erosion::
                ApplyRiverNetworkIncision(
                    *material,
                    inputs.geology,
                    *sediment,
                    rivers));
    }

    if (inputs.processes.
            coastal.enabled)
    {
        if (!sediment.has_value())
        {
            sediment =
                terrain_erosion::
                    SedimentExchangePage(
                        result->key.
                            resolution,
                        result->
                            spacingMeters);
        }

        auto coastalConfig =
            inputs.processes.
                coastal;
        coastalConfig.water.
            seaLevelMeters =
                inputs.
                    seaLevelMeters;

        const auto coastal =
            terrain_water::
                SimulateCoastalProcess(
                    *material,
                    inputs.geology,
                    *sediment,
                    {},
                    {},
                    coastalConfig);

        material =
            std::make_shared<
                terrain_material_column::
                    MaterialColumnPage>(
                        coastal.material);
        sediment =
            coastal.
                sedimentExchange;
    }

    result->material =
        std::move(material);

    if (sediment.has_value())
    {
        result->sedimentExchange =
            std::make_shared<
                terrain_erosion::
                    SedimentExchangePage>(
                        std::move(
                            *sediment));
    }

    return result;
}

[[nodiscard]] f32 SlopeDegrees(
    const terrain_material_column::MaterialColumnPage& page,
    const u32 x,
    const u32 y)
{
    const u32 resolution =
        page.Resolution();

    const u32 west =
        x > 0U
            ? x - 1U
            : x;
    const u32 east =
        std::min(
            x + 1U,
            resolution - 1U);
    const u32 north =
        y > 0U
            ? y - 1U
            : y;
    const u32 south =
        std::min(
            y + 1U,
            resolution - 1U);

    const f64 dx =
        static_cast<f64>(
            east - west) *
        page.SpacingMeters();
    const f64 dy =
        static_cast<f64>(
            south - north) *
        page.SpacingMeters();

    const f64 dzdx =
        dx > 0.0
            ? (static_cast<f64>(
                   page.At(
                       east,
                       y).
                       SurfaceHeightMeters()) -
               static_cast<f64>(
                   page.At(
                       west,
                       y).
                       SurfaceHeightMeters())) /
                  dx
            : 0.0;

    const f64 dzdy =
        dy > 0.0
            ? (static_cast<f64>(
                   page.At(
                       x,
                       south).
                       SurfaceHeightMeters()) -
               static_cast<f64>(
                   page.At(
                       x,
                       north).
                       SurfaceHeightMeters())) /
                  dy
            : 0.0;

    return
        static_cast<f32>(
            std::atan(
                std::sqrt(
                    dzdx * dzdx +
                    dzdy * dzdy)) *
            180.0 /
            std::numbers::pi_v<f64>);
}

[[nodiscard]] BundlePtr BuildExposedSurface(
    const BodyBuildInputs& inputs,
    const terrain::TerrainGenerationRevisions& revisions,
    const procedural_graph::BuildContext& context)
{
    const BundlePtr upstream =
        DependencyBundle(
            context,
            0U);

    auto result =
        std::make_shared<PhysicalBundle>(
            *upstream);

    result->key.revisions =
        revisions;

    const u32 resolution =
        result->key.resolution;
    result->exposedSurface.resize(
        static_cast<std::size_t>(
            resolution) *
        resolution);

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const std::size_t index =
                CellIndex(
                    resolution,
                    x,
                    y);

            const auto& column =
                result->material->
                    At(x, y);

            result->exposedSurface[index] =
                terrain_material_column::
                    ResolveSurface(
                        column,
                        terrain_material_column::
                            SampleColumnGeology(
                                column,
                                inputs.geology));
        }
    }

    return result;
}

[[nodiscard]] BundlePtr BuildBiomeWeights(
    const BodyBuildInputs& inputs,
    const terrain::TerrainGenerationRevisions& revisions,
    const procedural_graph::BuildContext& context)
{
    const BundlePtr upstream =
        DependencyBundle(
            context,
            0U);

    auto result =
        std::make_shared<PhysicalBundle>(
            *upstream);

    result->key.revisions =
        revisions;

    const u32 resolution =
        result->key.resolution;
    const std::size_t count =
        static_cast<std::size_t>(
            resolution) *
        resolution;

    result->biomeWeights.resize(
        count);
    result->dominantBiomeWeights.assign(
        count,
        0.0F);
    result->dominantBiomes.assign(
        count,
        inputs.biomes.
            BaseBiome().
            id);

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const std::size_t index =
                CellIndex(
                    resolution,
                    x,
                    y);

            const auto& source =
                result->
                    sourceSamples[index];
            const auto& column =
                result->material->
                    At(x, y);

            const f32 slope =
                SlopeDegrees(
                    *result->material,
                    x,
                    y);

            const auto drainage =
                result->drainage != nullptr
                    ? result->drainage->
                          At(x, y).
                          drainageAreaSquareMeters
                    : 0.0;

            const auto contextValue =
                terrain_biome::
                    BiomePlacementContext{
                        .unitDirection =
                            result->
                                positions[index].
                                unitDirection,
                        .planetRadiusMeters =
                            inputs.planet.
                                radiusMeters,
                        .temperatureC =
                            source.climate.
                                temperatureC,
                        .moisture =
                            column.moisture,
                        .rainfall =
                            source.climate.
                                precipitation,
                        .elevationMeters =
                            column.
                                SurfaceHeightMeters(),
                        .slopeDegrees =
                            slope,
                        .aspectRadians =
                            0.0,
                        .latitudeRadians =
                            std::asin(
                                std::clamp(
                                    result->
                                        positions[index].
                                        unitDirection.y,
                                    -1.0,
                                    1.0)),
                        .continentality =
                            source.climate.
                                continentality,
                        .distanceToCoastWaterMeters =
                            0.0,
                        .drainage =
                            drainage,
                        .soilDepthMeters =
                            column.soilMeters,
                        .sandDepthMeters =
                            column.sandMeters,
                        .substrateRock =
                            column.
                                bedrockMaterial,
                        .solarExposure =
                            0.5,
                        .windExposure =
                            0.5,
                        .snowPersistence =
                            0.0
                    };

            auto resolved =
                inputs.biomes.
                    ResolvePlacement(
                        contextValue);

            f32 dominantWeight =
                -1.0F;
            terrain_biome::BiomeId
                dominant =
                    inputs.biomes.
                        BaseBiome().
                        id;

            for (const auto& weight :
                 resolved)
            {
                if (weight.weight >
                    dominantWeight)
                {
                    dominantWeight =
                        weight.weight;
                    dominant =
                        weight.id;
                }
            }

            result->
                dominantBiomeWeights[index] =
                    std::max(
                        dominantWeight,
                        0.0F);
            result->
                dominantBiomes[index] =
                    dominant;
            result->
                biomeWeights[index] =
                    std::move(
                        resolved);
        }
    }

    return result;
}

[[nodiscard]] BundlePtr BuildSurfaceMaterial(
    const BodyBuildInputs& inputs,
    const terrain::TerrainGenerationRevisions& revisions,
    const procedural_graph::BuildContext& context)
{
    const BundlePtr exposed =
        DependencyBundle(
            context,
            0U);
    const BundlePtr biomes =
        DependencyBundle(
            context,
            1U);

    auto result =
        std::make_shared<PhysicalBundle>(
            *biomes);

    result->key.revisions =
        revisions;

    const u32 resolution =
        result->key.resolution;
    const std::size_t count =
        static_cast<std::size_t>(
            resolution) *
        resolution;

    result->surfaceMaterials.resize(
        count);

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const std::size_t index =
                CellIndex(
                    resolution,
                    x,
                    y);

            result->
                surfaceMaterials[index] =
                    surface_model::
                        ResolveSurfaceMaterialBlend(
                            exposed->
                                exposedSurface[index],
                            inputs.biomes,
                            result->
                                biomeWeights[index],
                            {
                                .slopeDegrees =
                                    SlopeDegrees(
                                        *result->
                                            material,
                                        x,
                                        y)
                            });
        }
    }

    return result;
}

[[nodiscard]] f32 BiomeWeight(
    const std::vector<
        terrain_biome::ResolvedBiomeWeight>& weights,
    const terrain_biome::BiomeId id) noexcept
{
    for (const auto& value :
         weights)
    {
        if (value.id == id)
        {
            return value.weight;
        }
    }

    return 0.0F;
}

[[nodiscard]] BundlePtr BuildScatter(
    const BodyBuildInputs& inputs,
    const terrain::TerrainGenerationRevisions& revisions,
    const procedural_graph::BuildContext& context)
{
    const BundlePtr exposed =
        DependencyBundle(
            context,
            0U);
    const BundlePtr biomes =
        DependencyBundle(
            context,
            1U);

    auto result =
        std::make_shared<PhysicalBundle>(
            *biomes);

    result->key.revisions =
        revisions;

    const u32 resolution =
        result->key.resolution;
    const std::size_t count =
        static_cast<std::size_t>(
            resolution) *
        resolution;

    result->
        scatterDensityPerSquareMeter.
            assign(
                count,
                0.0F);

    const auto definitions =
        inputs.biomes.
            Definitions();

    for (const auto& biome :
         definitions)
    {
        for (const auto& rule :
             biome.scatter.layers)
        {
            if (!rule.enabled)
            {
                continue;
            }

            const f32 cellSize =
                static_cast<f32>(
                    std::max(
                        result->
                            spacingMeters,
                        static_cast<f64>(
                            rule.
                                minimumSpacingMeters)));

            terrain_scatter::
                ScatterPageRequest request{
                    .identity = {
                        .planet =
                            result->key.
                                address.
                                planet,
                        .tile =
                            result->key.
                                address.
                                tile,
                        .sourceRevision =
                            terrain::
                                RevisionFingerprint(
                                    revisions),
                        .scatterRevision =
                            revisions.biome,
                        .generationSeed =
                            terrain::
                                DeriveTerrainSeed(
                                    inputs.planet.
                                        generationSeed,
                                    terrain::
                                        TerrainSeedDomain::
                                            Biome,
                                    result->key.
                                        address,
                                    rule.id.high,
                                    rule.id.low)
                    },
                    .gridResolution =
                        resolution,
                    .cellSizeMeters =
                        cellSize,
                    .rule =
                        rule,
                    .biomeDensityMultiplier =
                        biome.scatter.
                            densityMultiplier
                };

            std::vector<
                terrain_scatter::
                    ScatterCellInput>
                cells(
                    count);

            for (u32 y = 0U;
                 y < resolution;
                 ++y)
            {
                for (u32 x = 0U;
                     x < resolution;
                     ++x)
                {
                    const std::size_t index =
                        CellIndex(
                            resolution,
                            x,
                            y);

                    const auto physical =
                        terrain_scatter::
                            MakePhysicalSurfaceScatterInput(
                                exposed->
                                    exposedSurface[index]);

                    cells[index] = {
                        .biomeWeight =
                            BiomeWeight(
                                result->
                                    biomeWeights[index],
                                biome.id),
                        .exposedMaterial =
                            physical.material,
                        .slopeDegrees =
                            SlopeDegrees(
                                *result->
                                    material,
                                x,
                                y),
                        .soilDepthMeters =
                            result->
                                material->
                                At(x, y).
                                soilMeters,
                        .moisture =
                            physical.
                                moisture,
                        .exclusionMask =
                            0.0F,
                        .authoredDensity =
                            1.0F
                    };
                }
            }

            const auto instances =
                terrain_scatter::
                    GenerateDeterministicScatter(
                        request,
                        cells);

            const f32 inverseArea =
                1.0F /
                (cellSize *
                 cellSize);

            for (const auto& instance :
                 instances)
            {
                const auto index =
                    CellIndex(
                        resolution,
                        instance.cellX,
                        instance.cellY);

                result->
                    scatterDensityPerSquareMeter[
                        index] +=
                            inverseArea;
            }
        }
    }

    return result;
}

[[nodiscard]] std::any BuildProduct(
    const std::shared_ptr<
        SharedBodyInputs>& shared,
    const terrain::PhysicalTerrainPageAddress& address,
    const Product product,
    const u32 resolution,
    const terrain::TerrainGenerationRevisions& revisions,
    const procedural_graph::BuildContext& context)
{
    const auto inputs =
        shared->Capture();

    if (inputs == nullptr ||
        inputs->source == nullptr)
    {
        throw std::logic_error(
            "M12 physical page build lost its immutable body inputs.");
    }

    switch (product)
    {
    case Product::Geology:
        return std::any(
            BuildGeology(
                *inputs,
                address,
                resolution,
                revisions,
                context));
    case Product::Drainage:
        return std::any(
            BuildDrainage(
                *inputs,
                revisions,
                context));
    case Product::TerrainProcesses:
        return std::any(
            BuildProcesses(
                *inputs,
                revisions,
                context));
    case Product::ExposedSurface:
        return std::any(
            BuildExposedSurface(
                *inputs,
                revisions,
                context));
    case Product::BiomeWeights:
        return std::any(
            BuildBiomeWeights(
                *inputs,
                revisions,
                context));
    case Product::SurfaceMaterial:
        return std::any(
            BuildSurfaceMaterial(
                *inputs,
                revisions,
                context));
    case Product::Scatter:
        return std::any(
            BuildScatter(
                *inputs,
                revisions,
                context));
    case Product::Count:
        break;
    }

    throw std::invalid_argument(
        "M12 requested an invalid terrain dependency product.");
}

[[nodiscard]] std::shared_ptr<
    const BodyBuildInputs>
CaptureInputs(
    editor_session::EditorWorldSession& world,
    const StudioTerrainViewportRuntimeSnapshot& runtime)
{
    const auto* capability =
        world.Surfaces().
            Registry().
            FindTerrainSurface(
                runtime.body);
    const auto* services =
        world.Surfaces().
            ServicesForBody(
                runtime.body);
    const auto* constraints =
        world.Surfaces().
            ConstraintsForBody(
                runtime.body);

    if (capability == nullptr ||
        capability->terrain == nullptr ||
        services == nullptr ||
        constraints == nullptr)
    {
        return nullptr;
    }

    const auto analytic =
        std::dynamic_pointer_cast<
            const terrain::
                AnalyticTerrainSource>(
                    capability->
                        terrain);

    if (analytic == nullptr)
    {
        throw std::logic_error(
            "M12 production physical pages require the composed AnalyticTerrainSource.");
    }

    auto result =
        std::make_shared<
            BodyBuildInputs>(
                runtime.body);

    result->planet =
        runtime.planet;
    result->source =
        analytic;
    result->geology =
        services->Geology();
    result->defaultBedrock =
        services->DefaultBedrock();
    result->processes =
        services->Processes();
    result->biomes =
        services->Biomes();
    result->constraints =
        *constraints;
    result->seaLevelMeters =
        analytic->
            Description().
            global.
            seaLevelMeters;
    result->surfaceSourceRevision =
        runtime.
            surfaceSourceRevision;

    if (result->geology.Find(
            result->defaultBedrock) ==
        nullptr)
    {
        throw std::logic_error(
            "M12 default bedrock is missing from the composed geology library.");
    }

    return result;
}

[[nodiscard]] terrain::TerrainGenerationRevisions
InitialRevisions(
    const BodyBuildInputs& inputs)
{
    return {
        .geology =
            std::max<u64>(
                inputs.geology.
                    Revision(),
                1U),
        .climate =
            std::max<u64>(
                inputs.source->
                    Revision(),
                1U),
        .authoring =
            std::max<u64>(
                inputs.
                    surfaceSourceRevision,
                1U),
        .biome =
            std::max<u64>(
                inputs.biomes.
                    Revision(),
                1U),
        .water = 1U,
        .processes = 1U
    };
}
} // namespace

class StudioTerrainPhysicalPageService::Impl
{
public:
    struct PagePublication
    {
        u64 revisionFingerprint{0U};
        u64 invalidationRevision{0U};
        bool cacheResident{false};
        std::shared_ptr<
            const StudioTerrainPhysicalPageSnapshot>
            snapshot;
    };

    struct BodyRuntime
    {
        BodyRuntime(
            jobs::JobSystem& jobs,
            std::shared_ptr<
                SharedBodyInputs> sharedIn,
            terrain_gpu::PersistentGpuTerrainCache& cache,
            const StudioTerrainPhysicalPageConfig& config)
            : shared(
                  std::move(
                      sharedIn)),
              graph(jobs),
              dependencies(
                  graph,
                  [sharedState = shared,
                   resolution = config.resolution](
                      const terrain::PhysicalTerrainPageAddress& address,
                      const Product product,
                      const terrain::TerrainGenerationRevisions& revisions,
                      const procedural_graph::BuildContext& context)
                  {
                      return BuildProduct(
                          sharedState,
                          address,
                          product,
                          resolution,
                          revisions,
                          context);
                  },
                  &cache),
              scheduler(
                  dependencies,
                  {
                      .editDebounceSeconds =
                          config.
                              editDebounceSeconds,
                      .maxBuildRequestsPerTick =
                          config.
                              rebuildRequestsPerTick
                  }),
              cache(&cache)
        {
        }

        std::shared_ptr<
            SharedBodyInputs> shared;
        procedural_graph::ProceduralGraph
            graph;
        terrain_dependency::TerrainDependencyGraph
            dependencies;
        StudioTerrainRebuildScheduler
            scheduler;
        terrain_gpu::PersistentGpuTerrainCache*
            cache{nullptr};

        terrain::TerrainGenerationRevisions
            baseRevisions{};
        u64 surfaceSourceRevision{~u64{0}};

        std::vector<
            terrain_dependency::TerrainInvalidationRequest>
            history;

        std::unordered_map<
            terrain::PhysicalTerrainPageAddress,
            PagePublication,
            AddressHash>
            publications;
    };

    Impl(
        editor_session::EditorWorldSession& worldIn,
        terrain_debug::TerrainDebugLivePages& debugIn,
        StudioTerrainPhysicalPageConfig configIn)
        : world(&worldIn),
          debugPages(&debugIn),
          config(configIn)
    {
        if (!config.IsValid())
        {
            throw std::invalid_argument(
                "M12 physical page service configuration is invalid.");
        }
    }

    [[nodiscard]] BodyRuntime* FindBody(
        const world::PlanetId planet) noexcept
    {
        for (auto& [body, runtime] :
             bodies)
        {
            static_cast<void>(
                body);

            const auto inputs =
                runtime->
                    shared->
                    Capture();

            if (inputs != nullptr &&
                inputs->planet.id ==
                    planet)
            {
                return runtime.get();
            }
        }

        return nullptr;
    }

    [[nodiscard]] const BodyRuntime* FindBody(
        const world::PlanetId planet) const noexcept
    {
        for (const auto& [body, runtime] :
             bodies)
        {
            static_cast<void>(
                body);

            const auto inputs =
                runtime->
                    shared->
                    Capture();

            if (inputs != nullptr &&
                inputs->planet.id ==
                    planet)
            {
                return runtime.get();
            }
        }

        return nullptr;
    }

    [[nodiscard]] terrain::TerrainGenerationRevisions
    RevisionsForNewPage(
        const BodyRuntime& body,
        const terrain::PhysicalTerrainPageAddress& address) const
    {
        auto revisions =
            body.baseRevisions;

        for (const auto& change :
             body.history)
        {
            if (AddressMatches(
                    address,
                    change.scope))
            {
                IncrementRevision(
                    revisions,
                    change.kind);
            }
        }

        return revisions;
    }

    void RegisterInterest(
        BodyRuntime& body,
        const terrain::PhysicalTerrainPageAddress& address)
    {
        if (body.scheduler.
                ContainsPage(
                    address))
        {
            return;
        }

        body.scheduler.
            RegisterPage(
                address,
                RevisionsForNewPage(
                    body,
                    address));
    }

    void InvalidatePublished(
        BodyRuntime& body,
        const std::span<
            const terrain_dependency::TerrainInvalidationRequest>
            changes)
    {
        if (changes.empty())
        {
            return;
        }

        const auto statuses =
            body.scheduler.Catalog();

        for (const auto& status :
             statuses)
        {
            const bool affected =
                std::any_of(
                    changes.begin(),
                    changes.end(),
                    [&](const auto& change)
                    {
                        return AddressMatches(
                            status.address,
                            change.scope);
                    });

            if (!affected)
            {
                continue;
            }

            body.publications.erase(
                status.address);

            if (debugPages != nullptr)
            {
                static_cast<void>(
                    debugPages->Erase(
                        status.address));
            }
        }
    }

    void PublishReady(
        BodyRuntime& body)
    {
        const auto statuses =
            body.scheduler.
                Catalog();

        for (const auto& status :
             statuses)
        {
            if (status.state !=
                    TerrainRebuildState::
                        Ready ||
                status.revisionFingerprint ==
                    0U)
            {
                continue;
            }

            const auto revisions =
                body.dependencies.
                    Revisions(
                        status.address);

            const auto nodes =
                body.dependencies.
                    Nodes(
                        status.address);

            if (!revisions.has_value() ||
                !nodes.has_value())
            {
                continue;
            }

            const auto* product =
                body.graph.
                    Product<BundlePtr>(
                        nodes->scatter);

            if (product == nullptr ||
                *product == nullptr)
            {
                continue;
            }

            const BundlePtr bundle =
                *product;

            const terrain_gpu::
                PersistentGpuTerrainCacheKey
                cacheKey{
                    .address =
                        status.address,
                    .physicalLod =
                        bundle->
                            physicalLod,
                    .revisions =
                        *revisions
                };

            const bool cacheResident =
                body.cache != nullptr &&
                body.cache->
                    IsResident(
                        cacheKey);

            auto found =
                body.publications.
                    find(
                        status.address);

            if (found !=
                    body.publications.end() &&
                found->second.
                        revisionFingerprint ==
                    status.
                        revisionFingerprint &&
                found->second.
                        cacheResident ==
                    cacheResident)
            {
                continue;
            }

            PagePublication publication{};

            if (found !=
                body.publications.end())
            {
                publication =
                    found->second;
            }

            if (publication.
                    revisionFingerprint !=
                status.
                    revisionFingerprint)
            {
                ++publication.
                    invalidationRevision;
            }

            const terrain_debug::
                TerrainDebugLivePageInputs
                inputs{
                    .address =
                        status.address,
                    .physicalLod =
                        bundle->
                            physicalLod,
                    .revisions =
                        *revisions,
                    .cacheResident =
                        cacheResident,
                    .invalidationRevision =
                        publication.
                            invalidationRevision,
                    .width =
                        bundle->
                            key.
                            resolution,
                    .height =
                        bundle->
                            key.
                            resolution,
                    .materialColumn =
                        bundle->
                            material.get(),
                    .drainage =
                        bundle->
                            drainage.get(),
                    .hydraulic =
                        bundle->
                            hydraulic.get(),
                    .sedimentExchange =
                        bundle->
                            sedimentExchange.get(),
                    .macroGeology =
                        bundle->
                            macroGeology,
                    .aeolianForcing =
                        bundle->
                            aeolianForcing,
                    .aeolian =
                        bundle->
                            aeolian.get(),
                    .dominantBiomeWeights =
                        bundle->
                            dominantBiomeWeights,
                    .dominantBiomes =
                        bundle->
                            dominantBiomes,
                    .scatterDensityPerSquareMeter =
                        bundle->
                            scatterDensityPerSquareMeter
                };

            const auto debug =
                terrain_debug::
                    CaptureLiveTerrainDebugPage(
                        inputs);

            if (!debugPages->Publish(
                    debug))
            {
                continue;
            }

            auto snapshot =
                std::make_shared<
                    StudioTerrainPhysicalPageSnapshot>();

            snapshot->address =
                status.address;
            snapshot->physicalLod =
                bundle->
                    physicalLod;
            snapshot->revisions =
                *revisions;
            snapshot->
                invalidationRevision =
                    publication.
                        invalidationRevision;
            snapshot->
                revisionFingerprint =
                    status.
                        revisionFingerprint;
            snapshot->
                cacheResident =
                    cacheResident;
            snapshot->material =
                bundle->
                    material;
            snapshot->debugPage =
                debug;

            publication.
                revisionFingerprint =
                    status.
                        revisionFingerprint;
            publication.
                cacheResident =
                    cacheResident;
            publication.snapshot =
                std::move(
                    snapshot);

            body.publications.
                insert_or_assign(
                    status.address,
                    std::move(
                        publication));
        }
    }

    editor_session::EditorWorldSession*
        world{nullptr};
    terrain_debug::TerrainDebugLivePages*
        debugPages{nullptr};
    StudioTerrainPhysicalPageConfig
        config{};

    jobs::JobSystem jobs{};

    std::unordered_map<
        universe::BodyId,
        std::unique_ptr<BodyRuntime>>
        bodies;

    bool paused{false};

    std::chrono::steady_clock::time_point
        lastTick{
            std::chrono::steady_clock::now()};
};

bool StudioTerrainPhysicalPageConfig::
IsValid() const noexcept
{
    return
        resolution >= 3U &&
        (resolution % 2U) == 1U &&
        rebuildRequestsPerTick > 0U &&
        std::isfinite(
            editDebounceSeconds) &&
        editDebounceSeconds >= 0.0;
}

StudioTerrainPhysicalPageService::
StudioTerrainPhysicalPageService(
    editor_session::EditorWorldSession& world,
    terrain_debug::TerrainDebugLivePages& debugPages,
    StudioTerrainPhysicalPageConfig config)
    : impl_(
          std::make_unique<Impl>(
              world,
              debugPages,
              config))
{
}

StudioTerrainPhysicalPageService::
~StudioTerrainPhysicalPageService() = default;

void StudioTerrainPhysicalPageService::Sync(
    const std::span<
        const StudioTerrainViewportRuntimeSnapshot>
        runtimes)
{
    if (impl_->world == nullptr ||
        !impl_->world->HasWorld())
    {
        Clear();
        return;
    }

    for (auto iterator =
             impl_->bodies.begin();
         iterator !=
             impl_->bodies.end();)
    {
        if (impl_->world->
                Surfaces().
                ServicesForBody(
                    iterator->first) ==
            nullptr)
        {
            iterator =
                impl_->bodies.erase(
                    iterator);
        }
        else
        {
            ++iterator;
        }
    }

    std::unordered_map<
        universe::BodyId,
        std::unordered_set<
            terrain::PhysicalTerrainPageAddress,
            AddressHash>>
        interests;

    for (const auto& runtime :
         runtimes)
    {
        auto inputs =
            CaptureInputs(
                *impl_->world,
                runtime);

        if (inputs == nullptr)
        {
            continue;
        }

        auto found =
            impl_->bodies.find(
                runtime.body);

        if (found ==
            impl_->bodies.end())
        {
            auto* services =
                impl_->world->
                    Surfaces().
                    ServicesForBody(
                        runtime.body);

            if (services == nullptr)
            {
                continue;
            }

            auto shared =
                std::make_shared<
                    SharedBodyInputs>();

            shared->Store(
                inputs);

            auto body =
                std::make_unique<
                    Impl::BodyRuntime>(
                        impl_->jobs,
                        std::move(
                            shared),
                        services->Cache(),
                        impl_->config);

            body->baseRevisions =
                InitialRevisions(
                    *inputs);
            body->
                surfaceSourceRevision =
                    runtime.
                        surfaceSourceRevision;
            body->scheduler.
                SetPaused(
                    impl_->paused);

            body->scheduler.
                SetAppliedChangeCallback(
                    [sharedState =
                         body->shared](
                        const terrain_dependency::
                            TerrainInvalidationRequest&,
                        const terrain_dependency::
                            TerrainInvalidationResult&)
                    {
                        sharedState->
                            CommitPending();
                    });

            found =
                impl_->bodies.
                    emplace(
                        runtime.body,
                        std::move(
                            body)).
                    first;
        }
        else if (found->second->
                     surfaceSourceRevision !=
                 runtime.
                     surfaceSourceRevision)
        {
            found->second->
                shared->
                Stage(
                    inputs);
            found->second->
                surfaceSourceRevision =
                    runtime.
                        surfaceSourceRevision;
        }

        auto& body =
            *found->second;

        auto& bodyInterests =
            interests[
                runtime.body];

        bodyInterests.insert(
            runtime.
                observerPhysicalPage);

        impl_->RegisterInterest(
            body,
            runtime.
                observerPhysicalPage);

        constexpr std::array<
            world::TileEdge,
            4U>
            edges{
                world::TileEdge::North,
                world::TileEdge::East,
                world::TileEdge::South,
                world::TileEdge::West
            };

        for (const auto edge :
             edges)
        {
            const auto neighbor =
                world::
                    NeighborAcrossTileEdge(
                        runtime.
                            observerPhysicalPage.
                            tile,
                        edge);

            const terrain::
                PhysicalTerrainPageAddress
                neighborAddress{
                    .planet =
                        runtime.
                            observerPhysicalPage.
                            planet,
                    .tile =
                        neighbor.tile
                };

            bodyInterests.insert(
                neighborAddress);

            impl_->RegisterInterest(
                body,
                neighborAddress);
        }
    }

    // Retire pages that no viewport currently needs. In-flight procedural
    // work is never blocked or cancelled here; UnregisterPage returns false
    // and the next Sync retries after that job completes.
    for (auto& [bodyId, body] :
         impl_->bodies)
    {
        const auto wanted =
            interests.find(
                bodyId);

        const auto statuses =
            body->scheduler.
                Catalog();

        for (const auto& status :
             statuses)
        {
            if (wanted !=
                    interests.end() &&
                wanted->second.contains(
                    status.address))
            {
                continue;
            }

            if (!body->scheduler.
                    UnregisterPage(
                        status.address))
            {
                continue;
            }

            body->publications.erase(
                status.address);

            if (impl_->debugPages !=
                nullptr)
            {
                static_cast<void>(
                    impl_->debugPages->
                        Erase(
                            status.address));
            }
        }
    }
}

void StudioTerrainPhysicalPageService::
QueueChange(
    const terrain_dependency::
        TerrainInvalidationRequest& request)
{
    if (!request.scope.IsValid())
    {
        throw std::invalid_argument(
            "M12 cannot queue an invalid terrain change.");
    }

    if (auto* body =
            impl_->FindBody(
                request.scope.
                    planet);
        body != nullptr)
    {
        body->scheduler.
            QueueChange(
                request);
    }
}

void StudioTerrainPhysicalPageService::
QueueChanges(
    const std::span<
        const terrain_dependency::
            TerrainInvalidationRequest>
        requests)
{
    for (const auto& request :
         requests)
    {
        QueueChange(
            request);
    }
}

void StudioTerrainPhysicalPageService::Tick()
{
    const auto now =
        std::chrono::steady_clock::
            now();

    const f64 delta =
        std::clamp(
            std::chrono::duration<f64>(
                now -
                impl_->lastTick).
                count(),
            0.0,
            0.25);

    impl_->lastTick =
        now;

    Tick(delta);
}

void StudioTerrainPhysicalPageService::Tick(
    const f64 deltaSeconds)
{
    for (auto& [bodyId, body] :
         impl_->bodies)
    {
        static_cast<void>(
            bodyId);

        body->scheduler.
            Tick(
                deltaSeconds);

        auto applied =
            body->scheduler.
                TakeAppliedChanges();

        impl_->InvalidatePublished(
            *body,
            applied);

        body->history.insert(
            body->history.end(),
            applied.begin(),
            applied.end());

        impl_->PublishReady(
            *body);
    }
}

void StudioTerrainPhysicalPageService::
RebuildDirty()
{
    for (auto& [bodyId, body] :
         impl_->bodies)
    {
        static_cast<void>(
            bodyId);

        body->scheduler.
            RebuildDirty();

        auto applied =
            body->scheduler.
                TakeAppliedChanges();

        impl_->InvalidatePublished(
            *body,
            applied);

        body->history.insert(
            body->history.end(),
            applied.begin(),
            applied.end());

        impl_->PublishReady(
            *body);
    }
}

void StudioTerrainPhysicalPageService::
SetPaused(
    const bool paused) noexcept
{
    impl_->paused =
        paused;

    for (auto& [bodyId, body] :
         impl_->bodies)
    {
        static_cast<void>(
            bodyId);
        body->scheduler.
            SetPaused(
                paused);
    }
}

bool StudioTerrainPhysicalPageService::
Paused() const noexcept
{
    return impl_->paused;
}

std::shared_ptr<
    const StudioTerrainPhysicalPageSnapshot>
StudioTerrainPhysicalPageService::Find(
    const terrain::PhysicalTerrainPageAddress& address) const
{
    const auto* body =
        impl_->FindBody(
            address.planet);

    if (body == nullptr)
    {
        return nullptr;
    }

    const auto found =
        body->publications.find(
            address);

    return found !=
            body->publications.end()
        ? found->second.
              snapshot
        : nullptr;
}

std::optional<u64>
StudioTerrainPhysicalPageService::BeginUpload(
    const terrain::PhysicalTerrainPageAddress& address)
{
    auto* body =
        impl_->FindBody(
            address.planet);

    return body != nullptr
        ? body->scheduler.
              BeginUpload(
                  address)
        : std::nullopt;
}

bool StudioTerrainPhysicalPageService::CompleteUpload(
    const terrain::PhysicalTerrainPageAddress& address,
    const u64 revisionFingerprint,
    const bool success,
    const std::string_view error)
{
    auto* body =
        impl_->FindBody(
            address.planet);

    return
        body != nullptr &&
        body->scheduler.
            CompleteUpload(
                address,
                revisionFingerprint,
                success,
                error);
}

std::optional<
    StudioTerrainPageRebuildStatus>
StudioTerrainPhysicalPageService::PageStatus(
    const terrain::PhysicalTerrainPageAddress& address) const
{
    const auto* body =
        impl_->FindBody(
            address.planet);

    return body != nullptr
        ? body->scheduler.
              PageStatus(
                  address)
        : std::nullopt;
}

std::optional<
    StudioTerrainBodyRebuildStatus>
StudioTerrainPhysicalPageService::BodyStatus(
    const world::PlanetId planet) const
{
    const auto* body =
        impl_->FindBody(
            planet);

    return body != nullptr
        ? std::optional<
              StudioTerrainBodyRebuildStatus>(
                  body->scheduler.
                      BodyStatus(
                          planet))
        : std::nullopt;
}

std::vector<StudioTerrainPageRebuildStatus>
StudioTerrainPhysicalPageService::Catalog(
    const world::PlanetId planet) const
{
    const auto* body =
        impl_->FindBody(
            planet);

    return body != nullptr
        ? body->scheduler.Catalog()
        : std::vector<
              StudioTerrainPageRebuildStatus>{};
}

void StudioTerrainPhysicalPageService::Clear()
{
    impl_->bodies.clear();

    if (impl_->debugPages != nullptr)
    {
        impl_->debugPages->
            Clear();
    }

    impl_->lastTick =
        std::chrono::steady_clock::
            now();
}
} // namespace orbit::studio_session
