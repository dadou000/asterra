#include <orbit/terrain_water/CoastalProcess.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace
{
using namespace orbit;
using namespace orbit::terrain_water;
using namespace orbit::terrain_material_column;

[[noreturn]] void Fail(const std::string& message)
{
    std::cerr
        << "M17 failure: "
        << message
        << '\n';

    std::exit(EXIT_FAILURE);
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
            terrain_geology::
                reference_rock::
                    Basalt,
        .name = "M17 competent basalt",
        .hardness = 0.97F,
        .cohesion = 0.94F,
        .hydraulicErodibility = 0.05F,
        .aeolianErodibility = 0.02F,
        .permeability = 0.05F,
        .chemicalWeatherability = 0.10F,
        .fractureTendency = 0.15F,
        .density = 3'050.0F
    });

    geology.Upsert({
        .id =
            terrain_geology::
                reference_rock::
                    VolcanicAsh,
        .name = "M17 weak coastal substrate",
        .hardness = 0.08F,
        .cohesion = 0.05F,
        .hydraulicErodibility = 0.95F,
        .aeolianErodibility = 0.70F,
        .permeability = 0.50F,
        .chemicalWeatherability = 0.85F,
        .fractureTendency = 0.90F,
        .density = 1'700.0F
    });

    return geology;
}

MaterialColumnCell Cell(
    const f32 bedrock,
    const terrain_geology::RockTypeId rock =
        terrain_geology::
            reference_rock::
                Basalt)
{
    return {
        .bedrockHeightMeters =
            bedrock,
        .referenceBedrockHeightMeters =
            bedrock,
        .bedrockMaterial =
            rock,
        .regolithMeters = 0.0F,
        .soilMeters = 0.0F,
        .sandMeters = 0.0F,
        .debrisMeters = 0.0F,
        .moisture = 0.0F,
        .temporaryScalar = 0.0F
    };
}

CoastalBoundaryState WestOpenBoundary(
    const u32 resolution,
    const f32 externalBed,
    const f32 surface,
    const f32 velocityEast = 0.0F,
    const f32 velocitySouth = 0.0F)
{
    CoastalBoundaryState result{};

    const CoastalBoundaryCell closed{
        .mode =
            CoastalBoundaryMode::
                ClosedWall,
        .bedElevationMeters =
            externalBed,
        .waterSurfaceElevationMeters =
            surface
    };

    result.north.assign(
        resolution,
        closed);

    result.east.assign(
        resolution,
        closed);

    result.south.assign(
        resolution,
        closed);

    result.west.assign(
        resolution,
        CoastalBoundaryCell{
            .mode =
                CoastalBoundaryMode::
                    OpenOcean,
            .bedElevationMeters =
                externalBed,
            .waterSurfaceElevationMeters =
                surface,
            .velocityEastMetersPerSecond =
                velocityEast,
            .velocitySouthMetersPerSecond =
                velocitySouth
        });

    result.revision = 17U;

    return result;
}

void TestClosedWaterConservation()
{
    constexpr u32 resolution = 9U;

    MaterialColumnPage material(
        resolution,
        3.0);

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            material.SetCell(
                x,
                y,
                Cell(-1.0F));
        }
    }

    CoastalShallowWaterConfig config{};
    config.seaLevelMeters = 0.0;
    config.wave.enabled = false;
    config.manningRoughness = 0.0;
    config.maximumTimeStepSeconds = 0.1;

    auto water =
        InitializeCoastalShallowWater(
            material,
            config);

    AdvanceCoastalShallowWater(
        water,
        material,
        {},
        config,
        80U);

    Require(
        water.balance.
            balanceRelativeError <
            1.0e-11,
        "M17 closed shallow-water page did not conserve water volume.");

    for (const auto& cell :
         water.cells)
    {
        RequireNear(
            cell.waterDepthMeters,
            1.0,
            1.0e-9,
            "M17 still-water state drifted over a flat closed bed.");

        Require(
            std::hypot(
                cell.
                    velocityMetersPerSecond.x,
                cell.
                    velocityMetersPerSecond.y) <
                1.0e-9,
            "M17 still-water state developed spurious current.");
    }
}

void TestShorelineTracksM08TerrainEdits()
{
    constexpr u32 resolution = 7U;

    MaterialColumnPage material(
        resolution,
        2.0);

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const f32 bed =
                x <= 2U
                    ? -0.75F
                    : 0.40F;

            material.SetCell(
                x,
                y,
                Cell(bed));
        }
    }

    CoastalShallowWaterConfig config{};
    config.seaLevelMeters = 0.0;
    config.wave.enabled = false;
    config.maximumTimeStepSeconds = 0.05;

    auto water =
        InitializeCoastalShallowWater(
            material,
            config);

    Require(
        water.At(2U, 3U).wet,
        "M17 shoreline setup expected the nearshore cell to be wet.");

    Require(
        water.At(2U, 3U).shoreline ||
        water.At(3U, 3U).shoreline,
        "M17 did not detect the terrain-defined initial shoreline.");

    static_cast<void>(
        material.Deposit(
            2U,
            3U,
            LooseMaterialKind::Sand,
            1.25));

    AdvanceCoastalShallowWater(
        water,
        material,
        {},
        config,
        1U);

    Require(
        !water.At(2U, 3U).wet,
        "M17 water did not dry after M08 terrain rose above the free surface.");

    Require(
        water.At(1U, 3U).shoreline ||
        water.At(2U, 3U).shoreline,
        "M17 shoreline did not move after the physical terrain edit.");
}

void TestOpenOceanWaveRespectsEmergentBarrier()
{
    constexpr u32 resolution = 11U;

    MaterialColumnPage material(
        resolution,
        2.0);

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            f32 bed =
                x < 5U
                    ? -1.5F
                    : 0.20F;

            if (x == 5U)
            {
                bed = 1.50F;
            }

            material.SetCell(
                x,
                y,
                Cell(bed));
        }
    }

    CoastalShallowWaterConfig config{};
    config.seaLevelMeters = 0.0;
    config.maximumTimeStepSeconds = 0.06;
    config.manningRoughness = 0.02;
    config.wave.enabled = true;
    config.wave.amplitudeMeters = 0.35;
    config.wave.periodSeconds = 3.5;
    config.wave.direction = {1.0, 0.0};

    auto water =
        InitializeCoastalShallowWater(
            material,
            config);

    const auto boundary =
        WestOpenBoundary(
            resolution,
            -1.5F,
            0.0F,
            0.35F,
            0.0F);

    AdvanceCoastalShallowWater(
        water,
        material,
        boundary,
        config,
        180U);

    f64 exposedVelocity = 0.0;

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        exposedVelocity =
            std::max(
                exposedVelocity,
                std::hypot(
                    water.
                        At(2U, y).
                        velocityMetersPerSecond.x,
                    water.
                        At(2U, y).
                        velocityMetersPerSecond.y));

        Require(
            !water.At(5U, y).wet,
            "M17 wave solver flooded an emergent above-sea barrier.");
    }

    Require(
        exposedVelocity > 0.02,
        "M17 open-ocean forcing did not generate a propagating shallow-water current.");

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        Require(
            !water.At(8U, y).wet,
            "M17 boundary wave propagated through a dry terrain barrier.");
    }
}

f64 SandCentroidX(
    const MaterialColumnPage& material,
    const terrain_erosion::SedimentExchangePage& sediment)
{
    f64 weighted = 0.0;
    f64 total = 0.0;

    const f64 area =
        material.
            CellAreaSquareMeters();

    const f64 density =
        material.
            Densities().
            sandKgPerCubicMeter;

    for (u32 y = 0U;
         y < material.Resolution();
         ++y)
    {
        for (u32 x = 0U;
             x < material.Resolution();
             ++x)
        {
            const f64 physical =
                static_cast<f64>(
                    material.
                        At(x, y).
                        sandMeters) *
                area *
                density;

            const auto& mobile =
                sediment.At(
                    x,
                    y);

            const f64 sand =
                physical +
                mobile.
                    waterborne.
                    sandKg +
                mobile.
                    surfaceMobile.
                    sandKg;

            weighted +=
                static_cast<f64>(x) *
                sand;

            total +=
                sand;
        }
    }

    return
        total > 0.0
            ? weighted /
                total
            : 0.0;
}

void TestBeachSedimentMigratesAndConservesMass()
{
    constexpr u32 resolution = 9U;
    constexpr f64 spacing = 2.0;

    auto geology =
        MakeGeology();

    MaterialColumnPage material(
        resolution,
        spacing);

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const f32 bed =
                -1.60F +
                static_cast<f32>(x) *
                    0.33F;

            material.SetCell(
                x,
                y,
                Cell(
                    bed,
                    terrain_geology::
                        reference_rock::
                            Basalt));

            if (x == 3U)
            {
                static_cast<void>(
                    material.Deposit(
                        x,
                        y,
                        LooseMaterialKind::Sand,
                        0.30));
            }
        }
    }

    terrain_erosion::SedimentExchangePage sediment(
        resolution,
        spacing);

    const f64 initialCentroid =
        SandCentroidX(
            material,
            sediment);

    CoastalProcessConfig config{};
    config.enabled = true;
    config.hydrodynamicSteps = 120U;

    config.water.seaLevelMeters = 0.0;
    config.water.maximumTimeStepSeconds = 0.05;
    config.water.manningRoughness = 0.018;
    config.water.wave.enabled = true;
    config.water.wave.amplitudeMeters = 0.20;
    config.water.wave.periodSeconds = 3.0;
    config.water.wave.direction = {1.0, 0.20};

    config.sediment.activeDepthMeters = 4.0;
    config.sediment.referenceEnergySquareMetersPerSecondSquared = 0.35;
    config.sediment.erosionMetersPerSecondAtReference = 0.025;
    config.sediment.maximumErosionDepthPerStepMeters = 0.015;
    config.sediment.transportRate = 5.0;
    config.sediment.maximumTransportFractionPerStep = 0.65;
    config.sediment.depositionVelocityThresholdMetersPerSecond = 1.2;
    config.sediment.depositionRatePerSecond = 0.8;
    config.sediment.shorelineDepositionMultiplier = 2.0;

    const auto boundary =
        WestOpenBoundary(
            resolution,
            -1.60F,
            0.0F,
            0.85F,
            0.12F);

    const auto result =
        SimulateCoastalProcess(
            std::move(material),
            geology,
            std::move(sediment),
            boundary,
            {},
            config);

    const f64 finalCentroid =
        SandCentroidX(
            result.material,
            result.
                sedimentExchange);

    Require(
        finalCentroid >
            initialCentroid +
                0.10,
        "M17 beach sand did not migrate down-current.");

    Require(
        result.diagnostics.
            transportedBedloadKg >
            0.0,
        "M17 did not transport any shared M14 coastal bedload.");

    Require(
        result.diagnostics.
            erodedMassKg >
            0.0,
        "M17 surf-zone coupling mobilized no physical beach sediment.");

    Require(
        result.diagnostics.
            materialBalanceRelativeError <
            5.0e-5,
        "M17 M08+M14 coastal sediment accounting did not conserve mass.");

    Require(
        result.diagnostics.
            shorelineCellCount >
            0U,
        "M17 coupled process lost its terrain-responsive shoreline.");
}

void TestDisabledProcessHasZeroTerrainEffect()
{
    constexpr u32 resolution = 5U;

    auto geology =
        MakeGeology();

    MaterialColumnPage material(
        resolution,
        3.0);

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            material.SetCell(
                x,
                y,
                Cell(
                    -0.5F +
                    static_cast<f32>(x) *
                        0.2F));

            if (x == 2U)
            {
                static_cast<void>(
                    material.Deposit(
                        x,
                        y,
                        LooseMaterialKind::Sand,
                        0.1));
            }
        }
    }

    terrain_erosion::SedimentExchangePage sediment(
        resolution,
        3.0);

    sediment.Add(
        1U,
        1U,
        terrain_erosion::
            SedimentTransportMedium::
                SurfaceMobile,
        terrain_erosion::SedimentMass{
            .sandKg = 25.0,
            .finesKg = 4.0,
            .coarseDebrisKg = 2.0
        });

    const auto originalMaterial =
        material;

    const auto originalSediment =
        sediment;

    CoastalProcessConfig config{};
    config.enabled = false;
    config.hydrodynamicSteps = 10U;

    const auto result =
        SimulateCoastalProcess(
            std::move(material),
            geology,
            std::move(sediment),
            {},
            {},
            config);

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const auto& before =
                originalMaterial.
                    At(x, y);

            const auto& after =
                result.material.
                    At(x, y);

            Require(
                before.bedrockHeightMeters ==
                        after.bedrockHeightMeters &&
                    before.regolithMeters ==
                        after.regolithMeters &&
                    before.soilMeters ==
                        after.soilMeters &&
                    before.sandMeters ==
                        after.sandMeters &&
                    before.debrisMeters ==
                        after.debrisMeters &&
                    before.moisture ==
                        after.moisture,
                "M17 disabled solver changed physical M08 terrain.");

            const auto& sedimentBefore =
                originalSediment.
                    At(x, y);

            const auto& sedimentAfter =
                result.
                    sedimentExchange.
                    At(x, y);

            Require(
                sedimentBefore.
                        waterborne.
                        sandKg ==
                    sedimentAfter.
                        waterborne.
                        sandKg &&
                sedimentBefore.
                        waterborne.
                        finesKg ==
                    sedimentAfter.
                        waterborne.
                        finesKg &&
                sedimentBefore.
                        surfaceMobile.
                        sandKg ==
                    sedimentAfter.
                        surfaceMobile.
                        sandKg &&
                sedimentBefore.
                        surfaceMobile.
                        finesKg ==
                    sedimentAfter.
                        surfaceMobile.
                        finesKg &&
                sedimentBefore.
                        surfaceMobile.
                        coarseDebrisKg ==
                    sedimentAfter.
                        surfaceMobile.
                        coarseDebrisKg,
                "M17 disabled solver changed M14 mobile sediment.");
        }
    }

    Require(
        result.water.cells.empty(),
        "M17 disabled solver executed hidden water work.");
}
} // namespace

int main()
{
    TestClosedWaterConservation();
    TestShorelineTracksM08TerrainEdits();
    TestOpenOceanWaveRespectsEmergentBarrier();
    TestBeachSedimentMigratesAndConservesMass();
    TestDisabledProcessHasZeroTerrainEffect();

    std::cout
        << "Orbit M17 coastal process tests passed.\n";

    return EXIT_SUCCESS;
}
