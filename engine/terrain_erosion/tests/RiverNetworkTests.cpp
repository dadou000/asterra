#include <orbit/terrain_erosion/RiverNetwork.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using namespace orbit;
using namespace orbit::terrain_erosion;
using namespace orbit::terrain_hydrology;
using namespace orbit::terrain_material_column;

[[noreturn]] void Fail(const std::string& message)
{
    std::cerr << "M16 failure: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

void Require(const bool condition, const std::string& message)
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
    if (std::abs(a - b) > tolerance)
    {
        Fail(
            message + " (" +
            std::to_string(a) + " vs " +
            std::to_string(b) + ")");
    }
}

terrain_geology::GeologicalMaterialLibrary MakeGeology()
{
    terrain_geology::GeologicalMaterialLibrary geology;

    geology.Upsert({
        .id = terrain_geology::reference_rock::VolcanicAsh,
        .name = "M16 erodible substrate",
        .hardness = 0.10F,
        .cohesion = 0.08F,
        .hydraulicErodibility = 0.95F,
        .aeolianErodibility = 0.60F,
        .permeability = 0.50F,
        .chemicalWeatherability = 0.80F,
        .fractureTendency = 0.80F,
        .density = 1'700.0F
    });

    return geology;
}

MaterialColumnCell Cell(const f32 height)
{
    return {
        .bedrockHeightMeters = height,
        .referenceBedrockHeightMeters = height,
        .bedrockMaterial =
            terrain_geology::reference_rock::VolcanicAsh,
        .regolithMeters = 0.0F,
        .soilMeters = 0.0F,
        .sandMeters = 0.0F,
        .debrisMeters = 0.0F,
        .moisture = 0.0F,
        .temporaryScalar = 0.0F
    };
}

terrain::PhysicalTerrainPageKey Key(
    const u32 resolution,
    const u64 processRevision = 1U)
{
    terrain::PhysicalTerrainPageKey key{};
    key.address.planet = {
        .high = 0x4D3136504C414E45ULL,
        .low = 0x5400000000000001ULL
    };
    key.address.tile.face = world::CubeFace::PositiveY;
    key.address.tile.level = 4U;
    key.address.tile.x = 3U;
    key.address.tile.y = 2U;
    key.resolution = resolution;
    key.revisions.geology = 4U;
    key.revisions.climate = 5U;
    key.revisions.authoring = 6U;
    key.revisions.processes = processRevision;
    return key;
}

DrainagePage MakeTwoBasinDrainage(
    const u64 processRevision = 1U)
{
    constexpr u32 resolution = 7U;
    constexpr f64 spacing = 10.0;

    MaterialColumnPage material(
        resolution,
        spacing);

    std::vector<DrainageCellInput> inputs(
        static_cast<std::size_t>(resolution) *
            resolution);

    for (u32 y = 0U; y < resolution; ++y)
    {
        for (u32 x = 0U; x < resolution; ++x)
        {
            f32 height = 0.0F;

            if (x < 3U)
            {
                height =
                    10.0F +
                    static_cast<f32>(x) *
                        5.0F +
                    static_cast<f32>(
                        std::abs(
                            static_cast<i32>(y) -
                            3)) *
                        0.05F;
            }
            else if (x > 3U)
            {
                height =
                    10.0F +
                    static_cast<f32>(
                        6U - x) *
                        5.0F +
                    static_cast<f32>(
                        std::abs(
                            static_cast<i32>(y) -
                            3)) *
                        0.05F;
            }
            else
            {
                height =
                    35.0F +
                    static_cast<f32>(
                        std::abs(
                            static_cast<i32>(y) -
                            3)) *
                        0.05F;
            }

            material.SetCell(
                x,
                y,
                Cell(height));

            inputs[
                static_cast<std::size_t>(y) *
                    resolution +
                x] = {
                    .runoffMetersPerSecond = 0.01F,
                    .authoredDrainage = 0.0F,
                    .outlet = false
                };
        }
    }

    DrainageRoutingConfig routing{};
    routing.depressionPolicy =
        DepressionRoutingPolicy::PreserveClosed;

    return BuildDrainagePage(
        material,
        Key(
            resolution,
            processRevision),
        inputs,
        {},
        routing);
}

RiverNetworkConfig NetworkConfig()
{
    RiverNetworkConfig config{};
    config.minimumDrainageAreaSquareMeters = 50.0;
    config.minimumDischargeCubicMetersPerSecond = 0.25;
    config.referenceDischargeCubicMetersPerSecond = 1.0;
    config.baseChannelWidthMeters = 3.0;
    config.minimumChannelWidthMeters = 1.0;
    config.maximumChannelWidthMeters = 20.0;
    config.widthDischargeExponent = 0.5;
    config.baseChannelDepthMeters = 1.0;
    config.minimumChannelDepthMeters = 0.2;
    config.maximumChannelDepthMeters = 8.0;
    config.depthDischargeExponent = 0.35;
    config.enableMeanders = true;
    config.meanderIterations = 3U;
    config.meanderTimeStep = 1.0;
    config.curvatureMigrationRate = 0.10;
    config.deterministicSeedMigrationRate = 0.05;
    config.maximumCenterlineOffsetWidths = 2.5;
    config.enableCutoffs = false;
    return config;
}

void TestM09ExtractionStableIdsAndSelection()
{
    const DrainagePage drainage =
        MakeTwoBasinDrainage(1U);

    auto config =
        NetworkConfig();

    const RiverNetwork a =
        BuildRiverNetwork(
            drainage,
            {},
            config);

    Require(
        !a.nodes.empty() &&
        !a.segments.empty(),
        "M16 failed to extract a directed river graph from M09 drainage.");

    Require(
        a.basins.size() >= 2U,
        "M16 test drainage did not preserve separate drainage basins.");

    f64 minimumQ =
        std::numeric_limits<f64>::max();
    f64 maximumQ = 0.0;
    f64 widthAtMinimum = 0.0;
    f64 widthAtMaximum = 0.0;

    for (const auto& node : a.nodes)
    {
        if (node.dischargeCubicMetersPerSecond < minimumQ)
        {
            minimumQ =
                node.dischargeCubicMetersPerSecond;
            widthAtMinimum =
                node.channelWidthMeters;
        }

        if (node.dischargeCubicMetersPerSecond > maximumQ)
        {
            maximumQ =
                node.dischargeCubicMetersPerSecond;
            widthAtMaximum =
                node.channelWidthMeters;
        }
    }

    Require(
        maximumQ > minimumQ &&
        widthAtMaximum > widthAtMinimum,
        "M16 channel width is not coupled to M09 discharge.");

    const auto& node =
        a.nodes[
            a.nodes.size() / 2U];

    const RiverSelection selection =
        SelectNearestRiver(
            a,
            node.channelOffsetMeters,
            0.25);

    Require(
        selection.HasNode() ||
        selection.HasSegment(),
        "M16 stable river graph cannot be selected by Studio-facing query.");

    const DrainagePage regeneratedDrainage =
        MakeTwoBasinDrainage(99U);

    const RiverNetwork regenerated =
        BuildRiverNetwork(
            regeneratedDrainage,
            {},
            config);

    Require(
        regenerated.FindNode(node.id) != nullptr,
        "M16 Studio node identity changed only because terrain process revision changed.");
}

void TestBasinLocalAuthoringInvalidation()
{
    const DrainagePage drainage =
        MakeTwoBasinDrainage();

    auto config =
        NetworkConfig();

    const RiverNetwork baseline =
        BuildRiverNetwork(
            drainage,
            {},
            config);

    Require(
        baseline.basins.size() >= 2U,
        "M16 basin-local invalidation test requires two basins.");

    const RiverBasinId edited =
        baseline.basins[0].id;

    const RiverBasinId untouched =
        baseline.basins[1].id;

    RiverConstraint constraint{};
    constraint.id = {
        .high = 0x4D3136434F4E5354ULL,
        .low = 1ULL
    };
    constraint.targetBasin = edited;
    constraint.kind = RiverConstraintKind::Attract;
    constraint.centerMeters = {0.0, 0.0};
    constraint.radiusMeters = 500.0;
    constraint.strength = 3.0;
    constraint.revision = 1U;

    const std::vector<RiverConstraint> constraints{
        constraint
    };

    const RiverNetwork authored =
        BuildRiverNetwork(
            drainage,
            constraints,
            config);

    const auto* baselineEdited =
        baseline.FindBasin(edited);

    const auto* baselineUntouched =
        baseline.FindBasin(untouched);

    const auto* authoredEdited =
        authored.FindBasin(edited);

    const auto* authoredUntouched =
        authored.FindBasin(untouched);

    Require(
        baselineEdited != nullptr &&
        baselineUntouched != nullptr &&
        authoredEdited != nullptr &&
        authoredUntouched != nullptr,
        "M16 basin states disappeared after local authoring.");

    Require(
        baselineEdited->revision !=
            authoredEdited->revision,
        "M16 local river constraint did not invalidate its target basin.");

    Require(
        baselineUntouched->revision ==
            authoredUntouched->revision,
        "M16 local river constraint invalidated an unrelated basin.");
}

void TestMeanderConstraintMovesCenterlineAndIncisionFollows()
{
    const DrainagePage drainage =
        MakeTwoBasinDrainage();

    auto config =
        NetworkConfig();

    const RiverNetwork baseline =
        BuildRiverNetwork(
            drainage,
            {},
            config);

    Require(
        !baseline.basins.empty(),
        "M16 meander test has no basin.");

    const RiverBasinId basin =
        baseline.basins.front().id;

    const auto basinNode =
        std::find_if(
            baseline.nodes.begin(),
            baseline.nodes.end(),
            [basin](const RiverNetworkNode& node)
            {
                return node.basin == basin;
            });

    Require(
        basinNode != baseline.nodes.end(),
        "M16 meander test could not find a node in target basin.");

    RiverConstraint trajectory{};
    trajectory.id = {
        .high = 0x4D31365452414A43ULL,
        .low = 2ULL
    };
    trajectory.targetBasin = basin;
    trajectory.kind = RiverConstraintKind::Trajectory;
    trajectory.centerMeters = {
        basinNode->drainageOffsetMeters.x,
        basinNode->drainageOffsetMeters.y + 8.0
    };
    trajectory.directionMeters = {1.0, 0.0};
    trajectory.radiusMeters = 500.0;
    trajectory.strength = 4.0;
    trajectory.revision = 1U;

    const std::vector<RiverConstraint> constraints{
        trajectory
    };

    const RiverNetwork evolved =
        BuildRiverNetwork(
            drainage,
            constraints,
            config);

    f64 maximumShift = 0.0;

    for (const auto& node : evolved.nodes)
    {
        if (node.basin != basin)
        {
            continue;
        }

        maximumShift =
            std::max(
                maximumShift,
                std::hypot(
                    node.channelOffsetMeters.x -
                        node.drainageOffsetMeters.x,
                    node.channelOffsetMeters.y -
                        node.drainageOffsetMeters.y));
    }

    Require(
        maximumShift > 0.25,
        "M16 authored trajectory did not move the physical channel centreline.");

    auto geology =
        MakeGeology();

    MaterialColumnPage material(
        evolved.resolution,
        evolved.spacingMeters);

    for (u32 y = 0U; y < evolved.resolution; ++y)
    {
        for (u32 x = 0U; x < evolved.resolution; ++x)
        {
            material.SetCell(
                x,
                y,
                Cell(40.0F));
        }
    }

    SedimentExchangePage sediment(
        evolved.resolution,
        evolved.spacingMeters);

    const auto before =
        material.QueryMass(geology);

    RiverIncisionConfig incision{};
    incision.valleyWidthMultiplier = 2.5;
    incision.maximumIncisionMetersPerBake = 4.0;

    const RiverIncisionResult result =
        ApplyRiverNetworkIncision(
            material,
            geology,
            sediment,
            evolved,
            incision);

    Require(
        result.affectedCells > 0U &&
        result.erodedMassKg > 0.0,
        "M16 evolved channel produced no coupled physical M08 incision.");

    Require(
        sediment.TotalMobileMass().TotalKg() >
            0.0,
        "M16 incision did not publish removed material into M14 waterborne sediment.");

    const auto after =
        material.QueryMass(geology);

    Require(
        after.excavatedBedrockKg >
            before.excavatedBedrockKg,
        "M16 river position was not coupled to physical terrain incision.");
}

void TestCutoffAndOxbowEvent()
{
    const DrainagePage drainage =
        MakeTwoBasinDrainage();

    auto config =
        NetworkConfig();

    config.enableMeanders = false;
    config.enableCutoffs = true;
    config.minimumCutoffPathNodes = 3U;
    config.cutoffDistanceWidths = 1'000.0;

    const RiverNetwork network =
        BuildRiverNetwork(
            drainage,
            {},
            config);

    Require(
        !network.cutoffEvents.empty(),
        "M16 enabled cutoff process produced no cutoff/oxbow event.");

    const auto& event =
        network.cutoffEvents.front();

    Require(
        event.newSegment.IsValid() &&
        event.oxbowSegments.size() >=
            config.minimumCutoffPathNodes,
        "M16 cutoff event did not preserve the bypassed oxbow segment set.");

    const auto* cutoff =
        network.FindSegment(
            event.newSegment);

    Require(
        cutoff != nullptr &&
        cutoff->active &&
        cutoff->cutoffSegment,
        "M16 cutoff event did not create an active replacement channel segment.");

    for (const RiverSegmentId id :
         event.oxbowSegments)
    {
        const auto* old =
            network.FindSegment(id);

        Require(
            old != nullptr &&
            !old->active,
            "M16 oxbow path remained active after cutoff.");
    }
}

void TestDeterminism()
{
    const DrainagePage drainage =
        MakeTwoBasinDrainage();

    auto config =
        NetworkConfig();

    const RiverNetwork a =
        BuildRiverNetwork(
            drainage,
            {},
            config);

    const RiverNetwork b =
        BuildRiverNetwork(
            drainage,
            {},
            config);

    Require(
        a.nodes.size() == b.nodes.size() &&
        a.segments.size() == b.segments.size() &&
        a.basins.size() == b.basins.size(),
        "M16 deterministic builds changed graph sizes.");

    for (std::size_t i = 0U; i < a.nodes.size(); ++i)
    {
        const auto& lhs = a.nodes[i];
        const auto& rhs = b.nodes[i];

        Require(
            lhs.id == rhs.id &&
            lhs.basin == rhs.basin &&
            lhs.channelOffsetMeters.x ==
                rhs.channelOffsetMeters.x &&
            lhs.channelOffsetMeters.y ==
                rhs.channelOffsetMeters.y &&
            lhs.channelWidthMeters ==
                rhs.channelWidthMeters &&
            lhs.channelDepthMeters ==
                rhs.channelDepthMeters,
            "M16 fixed inputs produced different river-node state.");
    }

    for (std::size_t i = 0U; i < a.basins.size(); ++i)
    {
        Require(
            a.basins[i].id == b.basins[i].id &&
            a.basins[i].revision ==
                b.basins[i].revision,
            "M16 fixed inputs produced different basin revision identity.");
    }
}
} // namespace

int main()
{
    TestM09ExtractionStableIdsAndSelection();
    TestBasinLocalAuthoringInvalidation();
    TestMeanderConstraintMovesCenterlineAndIncisionFollows();
    TestCutoffAndOxbowEvent();
    TestDeterminism();

    std::cout
        << "Orbit M16 river network/meander tests passed.\n";

    return EXIT_SUCCESS;
}
