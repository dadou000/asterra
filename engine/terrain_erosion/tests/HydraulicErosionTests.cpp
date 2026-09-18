#include <orbit/terrain_erosion/HydraulicErosion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
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
        << "M11 failure: "
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
    if (std::abs(a - b) > tolerance)
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
        .name = "M11 hard basalt",
        .hardness = 0.98F,
        .cohesion = 0.94F,
        .hydraulicErodibility = 0.08F,
        .aeolianErodibility = 0.02F,
        .permeability = 0.08F,
        .chemicalWeatherability = 0.15F,
        .fractureTendency = 0.25F,
        .density = 3'050.0F
    });

    geology.Upsert({
        .id =
            terrain_geology::reference_rock::VolcanicAsh,
        .name = "M11 soft ash",
        .hardness = 0.08F,
        .cohesion = 0.05F,
        .hydraulicErodibility = 0.98F,
        .aeolianErodibility = 0.85F,
        .permeability = 0.75F,
        .chemicalWeatherability = 0.85F,
        .fractureTendency = 0.80F,
        .density = 1'650.0F
    });

    return geology;
}

MaterialColumnCell MakeCell(
    const f32 bedrockHeightMeters,
    const terrain_geology::RockTypeId rock,
    const f32 regolithMeters = 0.0F,
    const f32 soilMeters = 0.0F)
{
    return {
        .bedrockHeightMeters =
            bedrockHeightMeters,
        .referenceBedrockHeightMeters =
            bedrockHeightMeters,
        .bedrockMaterial =
            rock,
        .regolithMeters =
            regolithMeters,
        .soilMeters =
            soilMeters,
        .sandMeters = 0.0F,
        .debrisMeters = 0.0F,
        .moisture = 0.0F,
        .temporaryScalar = 0.0F
    };
}

MaterialColumnPage MakeSlope(
    const u32 resolution,
    const f64 spacingMeters,
    const terrain_geology::RockTypeId rock,
    const f32 regolithMeters,
    const f32 soilMeters)
{
    MaterialColumnPage page(
        resolution,
        spacingMeters);

    for (u32 y = 0;
         y < resolution;
         ++y)
    {
        for (u32 x = 0;
             x < resolution;
             ++x)
        {
            page.SetCell(
                x,
                y,
                MakeCell(
                    30.0F -
                        static_cast<f32>(x) *
                            0.8F,
                    rock,
                    regolithMeters,
                    soilMeters));
        }
    }

    return page;
}

HydraulicErosionConfig
TestConfig()
{
    HydraulicErosionConfig config{};
    config.iterations = 80U;
    config.timeStepSeconds = 0.15;
    config.rainfallMetersPerSecond = 0.0025;
    config.gravityMetersPerSecondSquared = 9.81;
    config.pipeCrossSectionSquareMeters = 0.20;
    config.sedimentCapacityCoefficient = 1'200.0;
    config.maximumSedimentConcentrationKgPerCubicMeter = 1'500.0;
    config.erosionRatePerSecond = 1.0;
    config.depositionRatePerSecond = 1.4;
    config.maximumErosionDepthPerStepMeters = 0.04;
    config.maximumDepositionDepthPerStepMeters = 0.04;
    config.infiltrationMetersPerSecond = 0.00008;
    config.moistureCapacityDepthMeters = 0.12;
    config.evaporationRatePerSecond = 0.08;
    return config;
}

void TestMaterialMassConservation()
{
    constexpr u32 resolution = 7U;

    auto geology =
        MakeGeology();

    auto page =
        MakeSlope(
            resolution,
            10.0,
            terrain_geology::reference_rock::VolcanicAsh,
            0.20F,
            0.20F);

    std::vector<f32> sources(
        static_cast<std::size_t>(
            resolution) *
            resolution,
        0.0F);

    // Add a persistent uphill source so mobile sediment is transported along
    // the slope instead of only undergoing local rain splash.
    for (u32 y = 1U;
         y + 1U < resolution;
         ++y)
    {
        sources[
            static_cast<std::size_t>(y) *
                resolution] =
            0.006F;
    }

    const auto result =
        SimulateHydraulicErosion(
            std::move(page),
            geology,
            sources,
            TestConfig());

    Require(
        result.massBalance.
            totalErodedKg >
            1.0,
        "M11 did not erode measurable material.");

    Require(
        result.massBalance.
            totalDepositedKg >
            0.0,
        "M11 transported sediment was never deposited.");

    Require(
        result.massBalance.
            finalSuspendedKg >
            0.0,
        "M11 should retain some mobile suspended sediment while water remains.");

    Require(
        result.massBalance.
            materialBalanceRelativeError <
            1.0e-9,
        "M11 eroded material was created/deleted instead of conserved.");

    RequireNear(
        result.massBalance.
            totalErodedKg,
        result.massBalance.
                totalDepositedKg +
            result.massBalance.
                finalSuspendedKg,
        std::max(
            result.massBalance.
                totalErodedKg *
                1.0e-9,
            1.0e-6),
        "M11 solid/suspended material balance is not closed.");
}

void TestTransportAndDownstreamDeposition()
{
    constexpr u32 resolution = 7U;

    auto geology =
        MakeGeology();

    auto page =
        MakeSlope(
            resolution,
            8.0,
            terrain_geology::reference_rock::VolcanicAsh,
            0.15F,
            0.20F);

    std::vector<f32> sources(
        static_cast<std::size_t>(
            resolution) *
            resolution,
        0.0F);

    sources[
        static_cast<std::size_t>(
            resolution / 2U) *
        resolution] =
        0.015F;

    auto config =
        TestConfig();

    config.iterations = 120U;
    config.evaporationRatePerSecond =
        0.12;

    const auto result =
        SimulateHydraulicErosion(
            std::move(page),
            geology,
            sources,
            config);

    f64 uphillErosion = 0.0;
    f64 downstreamDeposition = 0.0;

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const auto& cell =
                result.At(x, y);

            if (x < resolution / 2U)
            {
                uphillErosion +=
                    cell.
                        cumulativeErodedKg;
            }

            if (x >=
                resolution - 2U)
            {
                downstreamDeposition +=
                    cell.
                        cumulativeDepositedKg;
            }
        }
    }

    Require(
        uphillErosion > 0.0,
        "M11 source region did not erode.");

    Require(
        downstreamDeposition > 0.0,
        "M11 sediment was not carried downslope and deposited near the closed outlet.");
}

void TestHardBedrockResistsLooseMaterial()
{
    constexpr u32 resolution = 5U;

    auto geology =
        MakeGeology();

    auto hard =
        MakeSlope(
            resolution,
            8.0,
            terrain_geology::reference_rock::Basalt,
            0.0F,
            0.0F);

    auto loose =
        MakeSlope(
            resolution,
            8.0,
            terrain_geology::reference_rock::Basalt,
            0.35F,
            0.25F);

    auto config =
        TestConfig();

    config.iterations = 90U;
    config.evaporationRatePerSecond =
        0.02;

    const auto hardResult =
        SimulateHydraulicErosion(
            std::move(hard),
            geology,
            {},
            config);

    const auto looseResult =
        SimulateHydraulicErosion(
            std::move(loose),
            geology,
            {},
            config);

    f64 hardDepth = 0.0;
    f64 looseDepth = 0.0;

    for (const auto& cell :
         hardResult.cells)
    {
        hardDepth +=
            cell.
                cumulativeErodedDepthMeters;
    }

    for (const auto& cell :
         looseResult.cells)
    {
        looseDepth +=
            cell.
                cumulativeErodedDepthMeters;
    }

    Require(
        looseDepth >
            hardDepth * 2.0 +
                1.0e-6,
        "M11 hard M02 bedrock did not resist incision more strongly than soil/regolith.");
}

void TestInfiltrationUpdatesPhysicalMoisture()
{
    constexpr u32 resolution = 4U;

    auto geology =
        MakeGeology();

    auto page =
        MakeSlope(
            resolution,
            10.0,
            terrain_geology::reference_rock::VolcanicAsh,
            0.10F,
            0.15F);

    auto config =
        TestConfig();

    config.iterations = 20U;
    config.rainfallMetersPerSecond =
        0.004;
    config.infiltrationMetersPerSecond =
        0.001;
    config.evaporationRatePerSecond =
        0.25;

    const auto result =
        SimulateHydraulicErosion(
            std::move(page),
            geology,
            {},
            config);

    bool sawMoisture = false;

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const auto& column =
                result.material.At(
                    x,
                    y);

            if (column.moisture >
                0.001F)
            {
                sawMoisture = true;
            }

            Require(
                column.moisture >=
                        0.0F &&
                    column.moisture <=
                        1.0F,
                "M11 infiltration produced invalid M08 moisture.");
        }
    }

    Require(
        sawMoisture,
        "M11 infiltration did not couple water into the M08 moisture channel.");
}

void TestDeterministicFixedInputs()
{
    constexpr u32 resolution = 5U;

    auto geology =
        MakeGeology();

    const auto base =
        MakeSlope(
            resolution,
            9.0,
            terrain_geology::reference_rock::VolcanicAsh,
            0.12F,
            0.18F);

    const auto config =
        TestConfig();

    const auto a =
        SimulateHydraulicErosion(
            base,
            geology,
            {},
            config);

    const auto b =
        SimulateHydraulicErosion(
            base,
            geology,
            {},
            config);

    Require(
        a.cells.size() ==
            b.cells.size(),
        "M11 deterministic runs produced different state sizes.");

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
                lhs.waterDepthMeters ==
                        rhs.waterDepthMeters &&
                    lhs.fluxWestCubicMetersPerSecond ==
                        rhs.fluxWestCubicMetersPerSecond &&
                    lhs.fluxEastCubicMetersPerSecond ==
                        rhs.fluxEastCubicMetersPerSecond &&
                    lhs.fluxNorthCubicMetersPerSecond ==
                        rhs.fluxNorthCubicMetersPerSecond &&
                    lhs.fluxSouthCubicMetersPerSecond ==
                        rhs.fluxSouthCubicMetersPerSecond &&
                    lhs.velocityMetersPerSecond.x ==
                        rhs.velocityMetersPerSecond.x &&
                    lhs.velocityMetersPerSecond.y ==
                        rhs.velocityMetersPerSecond.y &&
                    lhs.suspendedSedimentKg ==
                        rhs.suspendedSedimentKg &&
                    lhs.cumulativeErodedKg ==
                        rhs.cumulativeErodedKg &&
                    lhs.cumulativeDepositedKg ==
                        rhs.cumulativeDepositedKg,
                "M11 hydraulic state is not deterministic.");

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
                        regolithMeters ==
                        rhsMaterial.
                            regolithMeters &&
                    lhsMaterial.
                        soilMeters ==
                        rhsMaterial.
                            soilMeters &&
                    lhsMaterial.
                        sandMeters ==
                        rhsMaterial.
                            sandMeters &&
                    lhsMaterial.
                        debrisMeters ==
                        rhsMaterial.
                            debrisMeters &&
                    lhsMaterial.moisture ==
                        rhsMaterial.moisture,
                "M11 deterministic runs changed the physical column differently.");
        }
    }
}
} // namespace

int main()
{
    TestMaterialMassConservation();
    TestTransportAndDownstreamDeposition();
    TestHardBedrockResistsLooseMaterial();
    TestInfiltrationUpdatesPhysicalMoisture();
    TestDeterministicFixedInputs();

    std::cout
        << "Orbit M11 hydraulic erosion tests passed.\n";

    return EXIT_SUCCESS;
}
