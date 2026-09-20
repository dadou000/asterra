#include <orbit/terrain_erosion/AeolianErosion.hpp>

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
        << "M13 failure: "
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

terrain_geology::GeologicalMaterialLibrary
MakeGeology()
{
    terrain_geology::GeologicalMaterialLibrary geology;

    geology.Upsert({
        .id =
            terrain_geology::reference_rock::Basalt,
        .name = "M13 basalt",
        .hardness = 0.98F,
        .cohesion = 0.94F,
        .hydraulicErodibility = 0.08F,
        .aeolianErodibility = 0.05F,
        .permeability = 0.08F,
        .chemicalWeatherability = 0.15F,
        .fractureTendency = 0.22F,
        .density = 3'050.0F
    });

    geology.Upsert({
        .id =
            terrain_geology::reference_rock::VolcanicAsh,
        .name = "M13 soft rock",
        .hardness = 0.08F,
        .cohesion = 0.05F,
        .hydraulicErodibility = 0.98F,
        .aeolianErodibility = 0.95F,
        .permeability = 0.75F,
        .chemicalWeatherability = 0.85F,
        .fractureTendency = 0.92F,
        .density = 1'650.0F
    });

    return geology;
}

MaterialColumnCell Cell(
    const f32 bedrock,
    const terrain_geology::RockTypeId rock,
    const f32 sand = 0.0F,
    const f32 soil = 0.0F,
    const f32 moisture = 0.0F)
{
    return {
        .bedrockHeightMeters = bedrock,
        .referenceBedrockHeightMeters = bedrock,
        .bedrockMaterial = rock,
        .regolithMeters = 0.0F,
        .soilMeters = soil,
        .sandMeters = sand,
        .debrisMeters = 0.0F,
        .moisture = moisture,
        .temporaryScalar = 0.0F
    };
}

std::vector<AeolianCellForcing> EastWind(
    const u32 resolution,
    const f32 speed,
    const f32 resistance = 0.0F)
{
    return std::vector<AeolianCellForcing>(
        static_cast<std::size_t>(resolution) *
            resolution,
        AeolianCellForcing{
            .windEastMetersPerSecond = speed,
            .windNorthMetersPerSecond = 0.0F,
            .surfaceResistance = resistance
        });
}

AeolianErosionConfig TestConfig()
{
    AeolianErosionConfig config{};
    config.iterations = 120U;
    config.timeStepSeconds = 0.20;
    config.capacityCoefficient = 0.045;
    config.windSpeedExponent = 2.0;
    config.shadowRayCells = 4U;
    config.shadowStrength = 12.0;
    config.windwardExposureGain = 0.75;
    config.minimumExposure = 0.02;
    config.maximumExposure = 2.0;
    config.pickupRatePerSecond = 2.0;
    config.depositionRatePerSecond = 2.5;
    config.reptationFraction = 0.25;
    config.saltationRatePerSecond = 3.0;
    config.referenceSaltationWindMetersPerSecond = 12.0;
    config.maximumSandPickupDepthPerStepMeters = 0.03;
    config.maximumSoilPickupDepthPerStepMeters = 0.015;
    config.maximumDepositionDepthPerStepMeters = 0.05;
    config.moistureSuppressionExponent = 2.5;
    config.bedrockAbrasionMetersPerSecondAtReferenceWind = 1.0e-6;
    config.maximumBedrockAbrasionDepthPerStepMeters = 5.0e-5;
    return config;
}

void TestLeeDepositionBuildsDune()
{
    constexpr u32 resolution = 9U;
    constexpr f64 spacing = 5.0;
    constexpr f32 initialSand = 0.25F;

    auto geology =
        MakeGeology();

    MaterialColumnPage page(
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
            const f32 obstacle =
                (x == 4U &&
                 y >= 2U &&
                 y <= 6U)
                    ? 2.5F
                    : 0.0F;

            page.SetCell(
                x,
                y,
                Cell(
                    obstacle,
                    terrain_geology::
                        reference_rock::Basalt,
                    initialSand));
        }
    }

    auto config =
        TestConfig();

    const auto result =
        SimulateAeolianErosion(
            std::move(page),
            geology,
            EastWind(
                resolution,
                14.0F),
            config);

    const auto& windward =
        result.At(
            3U,
            resolution / 2U);

    const auto& lee =
        result.At(
            5U,
            resolution / 2U);

    Require(
        lee.exposure <
            windward.exposure * 0.5F,
        "M13 obstacle did not create a meaningful lee-side wind shadow.");

    const f64 leeSand =
        result.material.At(
            5U,
            resolution / 2U).
            sandMeters;

    Require(
        leeSand >
            static_cast<f64>(
                initialSand) +
            0.01,
        "M13 available sand did not build a lee-side depositional dune.");

    Require(
        lee.cumulativeDepositedKg >
            0.0,
        "M13 shadowed lee cell never deposited transported sediment.");

    Require(
        result.massBalance.
            materialBalanceRelativeError <
            3.0e-5,
        "M13 dune formation violated physical material conservation.");
}

void TestWetAndResistantSurfaceSuppressPickup()
{
    constexpr u32 resolution = 5U;
    constexpr f64 spacing = 5.0;

    auto geology =
        MakeGeology();

    const auto makePage =
        [&](const f32 moisture)
        {
            MaterialColumnPage page(
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
                    page.SetCell(
                        x,
                        y,
                        Cell(
                            0.0F,
                            terrain_geology::
                                reference_rock::Basalt,
                            0.35F,
                            0.0F,
                            moisture));
                }
            }

            return page;
        };

    auto config =
        TestConfig();

    config.iterations = 20U;

    const auto dry =
        SimulateAeolianErosion(
            makePage(0.0F),
            geology,
            EastWind(
                resolution,
                12.0F),
            config);

    const auto wet =
        SimulateAeolianErosion(
            makePage(0.95F),
            geology,
            EastWind(
                resolution,
                12.0F),
            config);

    const auto resistant =
        SimulateAeolianErosion(
            makePage(0.0F),
            geology,
            EastWind(
                resolution,
                12.0F,
                0.95F),
            config);

    f64 dryPickup = 0.0;
    f64 wetPickup = 0.0;
    f64 resistantPickup = 0.0;

    for (std::size_t i = 0U;
         i < dry.cells.size();
         ++i)
    {
        dryPickup +=
            dry.cells[i].
                cumulativeSandPickedKg;

        wetPickup +=
            wet.cells[i].
                cumulativeSandPickedKg;

        resistantPickup +=
            resistant.cells[i].
                cumulativeSandPickedKg;
    }

    Require(
        dryPickup >
            wetPickup * 20.0 +
                1.0e-6,
        "M13 high moisture did not strongly suppress sand pickup.");

    Require(
        dryPickup >
            resistantPickup * 10.0 +
                1.0e-6,
        "M13 surface/vegetation resistance did not strongly suppress pickup.");
}

void TestBedrockAbrasionIsMuchSlowerThanLooseTransport()
{
    constexpr u32 resolution = 5U;
    constexpr f64 spacing = 5.0;

    auto geology =
        MakeGeology();

    MaterialColumnPage rock(
        resolution,
        spacing);

    MaterialColumnPage sand(
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
            rock.SetCell(
                x,
                y,
                Cell(
                    0.0F,
                    terrain_geology::
                        reference_rock::VolcanicAsh));

            sand.SetCell(
                x,
                y,
                Cell(
                    0.0F,
                    terrain_geology::
                        reference_rock::VolcanicAsh,
                    0.40F));
        }
    }

    auto config =
        TestConfig();

    config.iterations = 30U;

    const auto rockResult =
        SimulateAeolianErosion(
            std::move(rock),
            geology,
            EastWind(
                resolution,
                18.0F),
            config);

    const auto sandResult =
        SimulateAeolianErosion(
            std::move(sand),
            geology,
            EastWind(
                resolution,
                18.0F),
            config);

    f64 abrasionDepth = 0.0;
    f64 pickupDepth = 0.0;

    const f64 area =
        spacing * spacing;

    for (const auto& cell :
         rockResult.cells)
    {
        abrasionDepth +=
            cell.
                cumulativeBedrockAbradedKg /
            (area * 1'650.0);
    }

    for (const auto& cell :
         sandResult.cells)
    {
        pickupDepth +=
            cell.
                cumulativeSandPickedKg /
            (area *
             sandResult.material.
                 Densities().
                 sandKgPerCubicMeter);
    }

    Require(
        abrasionDepth > 0.0,
        "M13 exposed erodible rock never abrades.");

    Require(
        pickupDepth >
            abrasionDepth * 50.0,
        "M13 bedrock abrasion is not sufficiently slower than loose sand transport.");
}

void TestSoilFinesTransport()
{
    constexpr u32 resolution = 5U;

    auto geology =
        MakeGeology();

    MaterialColumnPage page(
        resolution,
        5.0);

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
                    0.0F,
                    terrain_geology::
                        reference_rock::Basalt,
                    0.0F,
                    0.25F));
        }
    }

    auto config =
        TestConfig();

    config.iterations = 30U;

    const auto result =
        SimulateAeolianErosion(
            std::move(page),
            geology,
            EastWind(
                resolution,
                13.0F),
            config);

    f64 picked = 0.0;
    f64 airborneFines = 0.0;

    for (const auto& cell :
         result.cells)
    {
        picked +=
            cell.
                cumulativeSoilPickedKg;

        airborneFines +=
            cell.
                airborneFinesKg;
    }

    Require(
        picked > 0.0,
        "M13 exposed soil/fines were never picked up by wind.");

    Require(
        airborneFines > 0.0,
        "M13 soil/fines pickup did not enter the airborne transport lane.");
}


void TestSeededMobileSedimentContinues()
{
    constexpr u32 resolution = 5U;
    constexpr f64 spacing = 5.0;
    constexpr f64 seededSandKg = 1'000.0;

    auto geology =
        MakeGeology();

    MaterialColumnPage page(
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
            page.SetCell(
                x,
                y,
                Cell(
                    0.0F,
                    terrain_geology::
                        reference_rock::Basalt));
        }
    }

    SedimentExchangePage seeded(
        resolution,
        spacing);

    seeded.Add(
        0U,
        resolution / 2U,
        SedimentTransportMedium::Airborne,
        {
            .sandKg = seededSandKg
        });

    auto config =
        TestConfig();

    config.iterations = 2U;
    config.pickupRatePerSecond = 0.0;
    config.depositionRatePerSecond = 5.0;
    config.saltationRatePerSecond = 0.0;
    config.maximumDepositionDepthPerStepMeters = 1.0;
    config.maximumAvalancheDepthPerStepMeters = 0.0;
    config.bedrockAbrasionMetersPerSecondAtReferenceWind = 0.0;
    config.maximumBedrockAbrasionDepthPerStepMeters = 0.0;

    const auto result =
        SimulateAeolianErosion(
            std::move(page),
            geology,
            EastWind(
                resolution,
                0.0F),
            config,
            std::move(seeded));

    Require(
        std::abs(
            result.massBalance.
                initialMobileMassKg -
            seededSandKg) <
            1.0e-9,
        "M13 seeded M14 sediment was not included in the initial process mass ledger.");

    Require(
        result.sedimentExchange->
            TotalMobileMass().
            Empty(1.0e-9),
        "M13 calm receiver did not consume seeded airborne sand through normal deposition.");

    const auto finalMass =
        result.material.QueryMass(
            geology);

    Require(
        std::abs(
            finalMass.LooseMassKg() -
            seededSandKg) <
            1.0e-2,
        "M13 seeded mobile sand did not become physical M08 sand.");

    Require(
        result.massBalance.
            materialBalanceRelativeError <
            2.0e-6,
        "M13 seeded-mobile continuation violated the physical/mobile mass ledger.");
}


void TestDeterministicFixedInputs()
{
    constexpr u32 resolution = 6U;

    auto geology =
        MakeGeology();

    MaterialColumnPage page(
        resolution,
        6.0);

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
                    (x == 3U)
                        ? 1.0F
                        : 0.0F,
                    terrain_geology::
                        reference_rock::Basalt,
                    0.30F));
        }
    }

    const auto wind =
        EastWind(
            resolution,
            11.0F);

    auto config =
        TestConfig();

    config.iterations = 40U;

    const auto a =
        SimulateAeolianErosion(
            page,
            geology,
            wind,
            config);

    const auto b =
        SimulateAeolianErosion(
            page,
            geology,
            wind,
            config);

    Require(
        a.cells.size() ==
            b.cells.size(),
        "M13 deterministic runs produced different state sizes.");

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const auto& lhs =
                a.At(x, y);

            const auto& rhs =
                b.At(x, y);

            Require(
                lhs.exposure ==
                        rhs.exposure &&
                    lhs.capacityKgPerSquareMeter ==
                        rhs.capacityKgPerSquareMeter &&
                    lhs.airborneSandKg ==
                        rhs.airborneSandKg &&
                    lhs.airborneFinesKg ==
                        rhs.airborneFinesKg &&
                    lhs.cumulativeSandPickedKg ==
                        rhs.cumulativeSandPickedKg &&
                    lhs.cumulativeSoilPickedKg ==
                        rhs.cumulativeSoilPickedKg &&
                    lhs.cumulativeDepositedKg ==
                        rhs.cumulativeDepositedKg &&
                    lhs.cumulativeBedrockAbradedKg ==
                        rhs.cumulativeBedrockAbradedKg,
                "M13 aeolian process state is not deterministic.");

            const auto& lhsMaterial =
                a.material.At(
                    x,
                    y);

            const auto& rhsMaterial =
                b.material.At(
                    x,
                    y);

            Require(
                lhsMaterial.
                        bedrockHeightMeters ==
                        rhsMaterial.
                            bedrockHeightMeters &&
                    lhsMaterial.
                        soilMeters ==
                        rhsMaterial.
                            soilMeters &&
                    lhsMaterial.
                        sandMeters ==
                        rhsMaterial.
                            sandMeters,
                "M13 deterministic runs changed the physical column differently.");
        }
    }
}
} // namespace

int main()
{
    TestLeeDepositionBuildsDune();
    TestWetAndResistantSurfaceSuppressPickup();
    TestBedrockAbrasionIsMuchSlowerThanLooseTransport();
    TestSoilFinesTransport();
    TestSeededMobileSedimentContinues();
    TestDeterministicFixedInputs();

    std::cout
        << "Orbit M13 aeolian erosion tests passed.\n";

    return EXIT_SUCCESS;
}
