#include <orbit/terrain_erosion/SedimentExchange.hpp>
#include <orbit/terrain_erosion/ThermalErosion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace
{
using namespace orbit;
using namespace orbit::terrain_erosion;
using namespace orbit::terrain_material_column;

[[noreturn]] void Fail(
    const std::string& message)
{
    std::cerr
        << "M14 failure: "
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

terrain_geology::GeologicalMaterialLibrary
MakeGeology()
{
    terrain_geology::GeologicalMaterialLibrary geology;

    geology.Upsert({
        .id =
            terrain_geology::reference_rock::Basalt,
        .name = "M14 basalt",
        .hardness = 0.98F,
        .cohesion = 0.94F,
        .hydraulicErodibility = 0.08F,
        .aeolianErodibility = 0.05F,
        .permeability = 0.08F,
        .chemicalWeatherability = 0.15F,
        .fractureTendency = 0.20F,
        .density = 3'050.0F
    });

    geology.Upsert({
        .id =
            terrain_geology::reference_rock::VolcanicAsh,
        .name = "M14 weak fractured rock",
        .hardness = 0.06F,
        .cohesion = 0.04F,
        .hydraulicErodibility = 0.98F,
        .aeolianErodibility = 0.92F,
        .permeability = 0.80F,
        .chemicalWeatherability = 0.88F,
        .fractureTendency = 0.98F,
        .density = 1'650.0F
    });

    return geology;
}

MaterialColumnCell Cell(
    const f32 bedrock,
    const terrain_geology::RockTypeId rock,
    const f32 regolith = 0.0F,
    const f32 soil = 0.0F,
    const f32 sand = 0.0F,
    const f32 debris = 0.0F,
    const f32 moisture = 0.0F)
{
    return {
        .bedrockHeightMeters = bedrock,
        .referenceBedrockHeightMeters = bedrock,
        .bedrockMaterial = rock,
        .regolithMeters = regolith,
        .soilMeters = soil,
        .sandMeters = sand,
        .debrisMeters = debris,
        .moisture = moisture,
        .temporaryScalar = 0.0F
    };
}

void TestCanonicalMaterialClassification()
{
    auto geology =
        MakeGeology();

    MaterialColumnPage page(
        1U,
        2.0);

    page.SetCell(
        0U,
        0U,
        Cell(
            10.0F,
            terrain_geology::
                reference_rock::Basalt,
            0.20F,
            0.20F,
            0.20F,
            0.20F));

    const auto* rock =
        geology.Find(
            terrain_geology::
                reference_rock::Basalt);

    Require(
        rock != nullptr,
        "M14 geology setup failed.");

    const auto removal =
        page.Erode(
            0U,
            0U,
            0.90,
            geology);

    const auto hydraulic =
        ClassifyRemovedMaterial(
            removal,
            page,
            *rock,
            SedimentSourceProcess::
                Hydraulic);

    const auto aeolian =
        ClassifyRemovedMaterial(
            removal,
            page,
            *rock,
            SedimentSourceProcess::
                AeolianAbrasion);

    const auto thermal =
        ClassifyRemovedMaterial(
            removal,
            page,
            *rock,
            SedimentSourceProcess::
                ThermalFracture);

    RequireNear(
        hydraulic.TotalKg(),
        removal.removedMassKg,
        1.0e-7,
        "M14 hydraulic material classification changed mass.");

    RequireNear(
        aeolian.TotalKg(),
        removal.removedMassKg,
        1.0e-7,
        "M14 aeolian material classification changed mass.");

    RequireNear(
        thermal.TotalKg(),
        removal.removedMassKg,
        1.0e-7,
        "M14 thermal material classification changed mass.");

    Require(
        hydraulic.finesKg >
            hydraulic.sandKg,
        "M14 hydraulic bedrock conversion should prefer fines.");

    Require(
        aeolian.sandKg >
            hydraulic.sandKg,
        "M14 aeolian abrasion should be sand-richer than hydraulic detachment.");

    Require(
        thermal.coarseDebrisKg >
            hydraulic.coarseDebrisKg,
        "M14 thermal fracture did not convert bedrock to coarse debris.");
}

void TestHydraulicDepositBecomesWindTransportable()
{
    auto geology =
        MakeGeology();

    MaterialColumnPage page(
        3U,
        5.0);

    for (u32 y = 0U;
         y < 3U;
         ++y)
    {
        for (u32 x = 0U;
             x < 3U;
             ++x)
        {
            page.SetCell(
                x,
                y,
                Cell(
                    0.0F,
                    terrain_geology::
                        reference_rock::Basalt));
        }
    }

    SedimentExchangePage exchange(
        3U,
        5.0);

    exchange.Add(
        1U,
        1U,
        SedimentTransportMedium::
            Waterborne,
        SedimentMass{
            .sandKg = 800.0,
            .finesKg = 0.0,
            .coarseDebrisKg = 0.0});

    const auto deposited =
        exchange.DepositToColumn(
            page,
            1U,
            1U,
            SedimentTransportMedium::
                Waterborne);

    Require(
        deposited.deposited.sandKg >
            790.0,
        "M14 hydraulic-style waterborne sand did not deposit into M08.");

    Require(
        page.At(1U, 1U).
            sandMeters >
            0.0F,
        "M14 hydraulic deposition did not create physical M08 sand.");

    page.At(1U, 1U).
        moisture = 0.0F;

    const f64 sandDepth =
        page.At(1U, 1U).
            sandMeters;

    const auto picked =
        exchange.PickupFromColumn(
            page,
            geology,
            1U,
            1U,
            sandDepth,
            SedimentSourceProcess::
                AeolianAbrasion,
            SedimentTransportMedium::
                Airborne);

    Require(
        picked.sandKg >
            790.0,
        "M14 deposited hydraulic sand did not become wind-transportable.");

    Require(
        exchange.At(1U, 1U).
            airborne.sandKg >
            790.0,
        "M14 wind pickup was not published to the shared airborne lane.");
}

void TestWindCanExposeRockCoveredByHydraulicSediment()
{
    auto geology =
        MakeGeology();

    MaterialColumnPage page(
        1U,
        4.0);

    page.SetCell(
        0U,
        0U,
        Cell(
            3.0F,
            terrain_geology::
                reference_rock::Basalt));

    SedimentExchangePage exchange(
        1U,
        4.0);

    exchange.Add(
        0U,
        0U,
        SedimentTransportMedium::
            Waterborne,
        SedimentMass{
            .sandKg =
                0.12 *
                page.CellAreaSquareMeters() *
                page.Densities().
                    sandKgPerCubicMeter});

    static_cast<void>(
        exchange.DepositToColumn(
            page,
            0U,
            0U,
            SedimentTransportMedium::
                Waterborne));

    Require(
        page.At(0U, 0U).
            ExposedSurface() ==
            ExposedSurfaceKind::Sand,
        "M14 waterborne deposit did not cover bedrock.");

    const f64 depth =
        page.At(0U, 0U).
            sandMeters;

    static_cast<void>(
        exchange.PickupFromColumn(
            page,
            geology,
            0U,
            0U,
            depth,
            SedimentSourceProcess::
                AeolianAbrasion,
            SedimentTransportMedium::
                Airborne));

    Require(
        page.At(0U, 0U).
            ExposedSurface() ==
            ExposedSurfaceKind::Bedrock,
        "M14 wind pickup could not re-expose bedrock beneath hydraulic sediment.");
}

void TestThermalCollapseFeedsLaterTransport()
{
    constexpr u32 resolution = 3U;

    auto geology =
        MakeGeology();

    MaterialColumnPage page(
        resolution,
        1.0);

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            page.SetCell(
                x,
                y,
                Cell(
                    (x == 1U &&
                     y == 1U)
                        ? 12.0F
                        : 0.0F,
                    terrain_geology::
                        reference_rock::
                            VolcanicAsh));
        }
    }

    ThermalErosionConfig config{};
    config.maximumIterations = 12U;
    config.bedrockFractureRate = 0.9;
    config.maximumTransferDepthPerIterationMeters =
        0.5;

    auto thermal =
        SimulateThermalErosion(
            std::move(page),
            geology,
            {},
            config);

    u32 debrisX = 0U;
    u32 debrisY = 0U;
    f32 maximumDebris = 0.0F;

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const f32 debris =
                thermal.material.
                    At(x, y).
                    debrisMeters;

            if (debris >
                maximumDebris)
            {
                maximumDebris =
                    debris;
                debrisX = x;
                debrisY = y;
            }
        }
    }

    Require(
        maximumDebris > 0.0F,
        "M14 setup did not receive debris from M12 thermal collapse.");

    SedimentExchangePage exchange(
        resolution,
        1.0);

    const auto picked =
        exchange.PickupFromColumn(
            thermal.material,
            geology,
            debrisX,
            debrisY,
            maximumDebris,
            SedimentSourceProcess::
                Hydraulic,
            SedimentTransportMedium::
                Waterborne);

    Require(
        picked.coarseDebrisKg >
            0.0,
        "M14 thermal talus did not enter later shared coarse-debris transport.");

    Require(
        exchange.At(
            debrisX,
            debrisY).
            waterborne.
            coarseDebrisKg >
            0.0,
        "M14 thermal debris was not published to the shared waterborne lane.");
}

void TestDepositionPriority()
{
    auto geology =
        MakeGeology();

    MaterialColumnPage page(
        1U,
        1.0);

    page.SetCell(
        0U,
        0U,
        Cell(
            0.0F,
            terrain_geology::
                reference_rock::Basalt));

    SedimentExchangePage exchange(
        1U,
        1.0);

    exchange.Add(
        0U,
        0U,
        SedimentTransportMedium::
            SurfaceMobile,
        SedimentMass{
            .sandKg = 100.0,
            .finesKg = 100.0,
            .coarseDebrisKg = 100.0});

    const auto result =
        exchange.DepositToColumn(
            page,
            0U,
            0U,
            SedimentTransportMedium::
                SurfaceMobile,
            150.0);

    RequireNear(
        result.deposited.coarseDebrisKg,
        100.0,
        1.0e-6,
        "M14 constrained deposition did not settle coarse debris first.");

    RequireNear(
        result.deposited.sandKg,
        50.0,
        1.0e-6,
        "M14 constrained deposition did not settle sand second.");

    RequireNear(
        result.deposited.finesKg,
        0.0,
        1.0e-9,
        "M14 fines consumed deposition budget before coarser sediment.");

    Require(
        page.At(0U, 0U).
            debrisMeters >
            0.0F &&
        page.At(0U, 0U).
            sandMeters >
            0.0F &&
        page.At(0U, 0U).
            soilMeters ==
            0.0F,
        "M14 deposition priority did not map to the expected M08 layers.");
}

void TestCrossPageBoundaryFlux()
{
    constexpr u32 resolution = 4U;

    SedimentExchangePage source(
        resolution,
        10.0);

    SedimentExchangePage neighbor(
        resolution,
        10.0);

    const SedimentMass payload{
        .sandKg = 12.0,
        .finesKg = 5.0,
        .coarseDebrisKg = 3.0};

    source.Add(
        resolution - 1U,
        2U,
        SedimentTransportMedium::
            Airborne,
        payload);

    const auto exported =
        source.ExportAcrossBoundary(
            resolution - 1U,
            2U,
            static_cast<i32>(
                resolution),
            2,
            SedimentTransportMedium::
                Airborne,
            payload);

    RequireNear(
        exported.TotalKg(),
        payload.TotalKg(),
        1.0e-12,
        "M14 edge export did not remove the requested typed mass.");

    const auto outgoing =
        source.
            TakeOutgoingBoundaryFlux(
                77U);

    Require(
        outgoing.IsComplete(
            resolution),
        "M14 outgoing edge packet is incomplete.");

    SedimentBoundaryFlux incoming{
        .north =
            std::vector<SedimentTransportPacket>(
                resolution),
        .east =
            std::vector<SedimentTransportPacket>(
                resolution),
        .south =
            std::vector<SedimentTransportPacket>(
                resolution),
        .west =
            outgoing.east,
        .revision =
            outgoing.revision};

    neighbor.ImportBoundaryFlux(
        incoming);

    const auto& received =
        neighbor.At(
            0U,
            2U).
            airborne;

    RequireNear(
        received.sandKg,
        payload.sandKg,
        1.0e-12,
        "M14 cross-page sand flux changed mass.");

    RequireNear(
        received.finesKg,
        payload.finesKg,
        1.0e-12,
        "M14 cross-page fines flux changed mass.");

    RequireNear(
        received.coarseDebrisKg,
        payload.coarseDebrisKg,
        1.0e-12,
        "M14 cross-page coarse-debris flux changed mass.");

    RequireNear(
        source.TotalMobileMass().
            TotalKg(),
        0.0,
        1.0e-12,
        "M14 source retained mass after boundary export.");

    RequireNear(
        neighbor.TotalMobileMass().
            TotalKg(),
        payload.TotalKg(),
        1.0e-12,
        "M14 neighbor did not receive the full boundary payload.");
}

void TestCornerBoundaryFlux()
{
    constexpr u32 resolution = 3U;

    SedimentExchangePage source(
        resolution,
        2.0);

    source.Add(
        0U,
        0U,
        SedimentTransportMedium::
            Waterborne,
        SedimentMass{
            .sandKg = 2.0,
            .finesKg = 3.0,
            .coarseDebrisKg = 4.0});

    static_cast<void>(
        source.ExportAcrossBoundary(
            0U,
            0U,
            -1,
            -1,
            SedimentTransportMedium::
                Waterborne,
            SedimentMass{
                .sandKg = 2.0,
                .finesKg = 3.0,
                .coarseDebrisKg = 4.0}));

    const auto flux =
        source.
            TakeOutgoingBoundaryFlux(
                9U);

    RequireNear(
        flux.corners[0].
            waterborne.
            TotalKg(),
        9.0,
        1.0e-12,
        "M14 NW corner flux was not preserved explicitly.");
}

void TestGpuPacking()
{
    SedimentExchangePage page(
        2U,
        4.0);

    page.Add(
        1U,
        0U,
        SedimentTransportMedium::
            Waterborne,
        SedimentMass{
            .sandKg = 160.0,
            .finesKg = 80.0,
            .coarseDebrisKg = 40.0});

    page.Add(
        1U,
        0U,
        SedimentTransportMedium::
            Airborne,
        SedimentMass{
            .sandKg = 32.0,
            .finesKg = 16.0,
            .coarseDebrisKg = 0.0});

    const auto gpu =
        PackGpuSedimentExchangePage(
            page);

    Require(
        gpu.TexelCount() == 4U,
        "M14 GPU shared-sediment packing has the wrong texel count.");

    Require(
        gpu.PackedByteSize() ==
            4U *
            3U *
            sizeof(
                GpuSedimentMediumTexel),
        "M14 GPU shared-sediment packed size is incorrect.");

    const auto& water =
        gpu.waterborne[1U];

    RequireNear(
        water.sandKgPerSquareMeter,
        10.0,
        1.0e-6,
        "M14 GPU sand packing did not use kg/m2.");

    RequireNear(
        water.finesKgPerSquareMeter,
        5.0,
        1.0e-6,
        "M14 GPU fines packing did not use kg/m2.");

    RequireNear(
        water.coarseDebrisKgPerSquareMeter,
        2.5,
        1.0e-6,
        "M14 GPU coarse packing did not use kg/m2.");
}
} // namespace

int main()
{
    TestCanonicalMaterialClassification();
    TestHydraulicDepositBecomesWindTransportable();
    TestWindCanExposeRockCoveredByHydraulicSediment();
    TestThermalCollapseFeedsLaterTransport();
    TestDepositionPriority();
    TestCrossPageBoundaryFlux();
    TestCornerBoundaryFlux();
    TestGpuPacking();

    std::cout
        << "Orbit M14 unified sediment exchange tests passed.\n";

    return EXIT_SUCCESS;
}
