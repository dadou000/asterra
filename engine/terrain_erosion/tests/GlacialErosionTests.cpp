#include <orbit/terrain_erosion/GlacialErosion.hpp>
#include <orbit/terrain_erosion/HydraulicErosion.hpp>

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
        << "M15 failure: "
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
        .name = "M15 competent basalt",
        .hardness = 0.95F,
        .cohesion = 0.90F,
        .hydraulicErodibility = 0.08F,
        .aeolianErodibility = 0.03F,
        .permeability = 0.08F,
        .chemicalWeatherability = 0.12F,
        .fractureTendency = 0.20F,
        .density = 3'050.0F
    });

    geology.Upsert({
        .id =
            terrain_geology::reference_rock::VolcanicAsh,
        .name = "M15 weak fractured substrate",
        .hardness = 0.10F,
        .cohesion = 0.08F,
        .hydraulicErodibility = 0.92F,
        .aeolianErodibility = 0.70F,
        .permeability = 0.65F,
        .chemicalWeatherability = 0.80F,
        .fractureTendency = 0.95F,
        .density = 1'700.0F
    });

    return geology;
}

MaterialColumnCell Cell(
    const f32 bedrock,
    const terrain_geology::RockTypeId rock)
{
    return {
        .bedrockHeightMeters = bedrock,
        .referenceBedrockHeightMeters = bedrock,
        .bedrockMaterial = rock,
        .regolithMeters = 0.0F,
        .soilMeters = 0.0F,
        .sandMeters = 0.0F,
        .debrisMeters = 0.0F,
        .moisture = 0.0F,
        .temporaryScalar = 0.0F
    };
}

std::vector<GlacialClimateCell> ColdClimate(
    const u32 resolution,
    const f32 initialIceMeters)
{
    return std::vector<GlacialClimateCell>(
        static_cast<std::size_t>(
            resolution) *
            resolution,
        GlacialClimateCell{
            .meanAnnualTemperatureC = -8.0F,
            .snowfallMetersIceEquivalentPerYear = 0.5F,
            .initialIceThicknessMeters =
                initialIceMeters,
            .processMask = 1.0F,
            .protection = 0.0F
        });
}

GlacialErosionConfig TestConfig()
{
    GlacialErosionConfig config{};
    config.iterations = 28U;
    config.timeStepYears = 0.5;

    config.maximumGlacierTemperatureC = 1.0;
    config.temperatureTransitionC = 3.0;
    config.snowfallForFullEligibilityMetersPerYear = 0.3;
    config.accumulationEfficiency = 0.0;
    config.meltStartTemperatureC = 0.0;
    config.meltMetersIcePerYearPerDegreeC = 0.0;
    config.maximumAblationMetersPerYear = 0.0;

    config.minimumIceThicknessForFlowMeters = 0.5;
    config.referenceIceThicknessMeters = 25.0;
    config.referenceSurfaceSlope = 0.03;
    config.referenceFlowSpeedMetersPerYear = 45.0;
    config.iceThicknessFlowExponent = 1.5;
    config.surfaceSlopeFlowExponent = 1.0;
    config.maximumFlowSpeedMetersPerYear = 180.0;
    config.maximumFlowFractionPerStep = 0.30;

    config.minimumIceThicknessForErosionMeters = 2.0;
    config.basalErosionMetersPerYearAtReference = 0.08;
    config.lateralErosionFraction = 1.25;
    config.referenceErosionSpeedMetersPerYear = 30.0;
    config.referenceErosionIceThicknessMeters = 25.0;
    config.erosionSpeedExponent = 0.6;
    config.erosionThicknessExponent = 0.4;
    config.maximumErosionDepthPerStepMeters = 0.25;

    config.debrisTransportEfficiency = 0.85;
    config.moraineDepositionRatePerYear = 0.8;
    config.moraineThinIceThresholdMeters = 8.0;
    config.moraineStagnationSpeedMetersPerYear = 5.0;

    return config;
}

void TestClimateMaskIsHardProcessBoundary()
{
    constexpr u32 resolution = 5U;

    auto geology =
        MakeGeology();

    MaterialColumnPage page(
        resolution,
        4.0);

    std::vector<f32> originalHeight(
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
            const f32 height =
                20.0F -
                static_cast<f32>(y) *
                    1.5F;

            page.SetCell(
                x,
                y,
                Cell(
                    height,
                    terrain_geology::
                        reference_rock::
                            VolcanicAsh));

            originalHeight[
                static_cast<std::size_t>(y) *
                    resolution +
                x] =
                    height;
        }
    }

    auto climate =
        ColdClimate(
            resolution,
            20.0F);

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 3U;
             x < resolution;
             ++x)
        {
            climate[
                static_cast<std::size_t>(y) *
                    resolution +
                x].
                processMask = 0.0F;
        }
    }

    auto config =
        TestConfig();

    config.iterations = 16U;

    const auto result =
        SimulateGlacialErosion(
            std::move(page),
            geology,
            climate,
            config);

    bool activeErosion = false;

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

            if (x >= 3U)
            {
                Require(
                    result.At(x, y).
                            eligibility ==
                        0.0F,
                    "M15 masked cell retained non-zero glacier eligibility.");

                Require(
                    result.At(x, y).
                            iceThicknessMeters ==
                        0.0F,
                    "M15 masked cell retained glacier ice.");

                RequireNear(
                    result.material.At(x, y).
                        bedrockHeightMeters,
                    originalHeight[index],
                    1.0e-7,
                    "M15 changed terrain outside the valid climate mask.");
            }
            else if (
                result.material.At(x, y).
                    bedrockHeightMeters <
                originalHeight[index] -
                    1.0e-5F)
            {
                activeErosion = true;
            }
        }
    }

    Require(
        activeErosion,
        "M15 active climate region produced no glacial erosion.");
}

void TestIceFlowConservesVolume()
{
    constexpr u32 resolution = 6U;

    auto geology =
        MakeGeology();

    MaterialColumnPage page(
        resolution,
        10.0);

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
                    50.0F -
                        static_cast<f32>(y) *
                            3.0F,
                    terrain_geology::
                        reference_rock::Basalt));
        }
    }

    auto climate =
        ColdClimate(
            resolution,
            0.0F);

    for (u32 x = 0U;
         x < resolution;
         ++x)
    {
        climate[x].
            initialIceThicknessMeters =
                30.0F;
    }

    auto config =
        TestConfig();

    config.iterations = 18U;
    config.basalErosionMetersPerYearAtReference = 0.0;
    config.lateralErosionFraction = 0.0;
    config.moraineDepositionRatePerYear = 0.0;

    const auto result =
        SimulateGlacialErosion(
            std::move(page),
            geology,
            climate,
            config);

    Require(
        result.iceBalance.
            balanceRelativeError <
            1.0e-10,
        "M15 internal glacier flow did not conserve ice volume.");

    f64 downstreamIce = 0.0;

    for (u32 y = 1U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            downstreamIce +=
                result.At(x, y).
                    iceThicknessMeters;
        }
    }

    Require(
        downstreamIce > 1.0,
        "M15 glacier flow did not move ice down the terrain gradient.");
}

MaterialColumnPage MakeValley(
    const u32 resolution,
    const f64 spacing,
    const terrain_geology::RockTypeId rock)
{
    MaterialColumnPage page(
        resolution,
        spacing);

    const i32 center =
        static_cast<i32>(
            resolution / 2U);

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const f32 cross =
                static_cast<f32>(
                    std::abs(
                        static_cast<i32>(x) -
                        center)) *
                3.5F;

            const f32 longitudinal =
                35.0F -
                static_cast<f32>(y) *
                    1.75F;

            page.SetCell(
                x,
                y,
                Cell(
                    longitudinal +
                        cross,
                    rock));
        }
    }

    return page;
}

void TestGlacierWidensValleyUnlikeHydraulicIncision()
{
    constexpr u32 resolution = 9U;
    constexpr f64 spacing = 8.0;

    auto geology =
        MakeGeology();

    const auto initial =
        MakeValley(
            resolution,
            spacing,
            terrain_geology::
                reference_rock::
                    VolcanicAsh);

    auto climate =
        ColdClimate(
            resolution,
            0.0F);

    const i32 center =
        static_cast<i32>(
            resolution / 2U);

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const f64 crossRelief =
                static_cast<f64>(
                    std::abs(
                        static_cast<i32>(x) -
                        center)) *
                3.5;

            climate[
                static_cast<std::size_t>(y) *
                    resolution +
                x].
                initialIceThicknessMeters =
                    static_cast<f32>(
                        std::max(
                            32.0 -
                                crossRelief,
                            0.0));
        }
    }

    auto glacialConfig =
        TestConfig();

    glacialConfig.iterations = 24U;
    glacialConfig.basalErosionMetersPerYearAtReference = 0.12;
    glacialConfig.lateralErosionFraction = 1.5;
    glacialConfig.moraineDepositionRatePerYear = 0.0;

    const auto glacial =
        SimulateGlacialErosion(
            initial,
            geology,
            climate,
            glacialConfig);

    HydraulicErosionConfig hydraulicConfig{};
    hydraulicConfig.iterations = 24U;
    hydraulicConfig.timeStepSeconds = 0.20;
    hydraulicConfig.rainfallMetersPerSecond = 0.0025;
    hydraulicConfig.sedimentCapacityCoefficient = 700.0;
    hydraulicConfig.erosionRatePerSecond = 0.45;
    hydraulicConfig.depositionRatePerSecond = 0.25;
    hydraulicConfig.maximumErosionDepthPerStepMeters = 0.08;
    hydraulicConfig.maximumDepositionDepthPerStepMeters = 0.04;
    hydraulicConfig.evaporationRatePerSecond = 0.0;
    hydraulicConfig.infiltrationMetersPerSecond = 0.0;

    const auto hydraulic =
        SimulateHydraulicErosion(
            initial,
            geology,
            {},
            hydraulicConfig);

    const u32 y =
        resolution / 2U;

    const u32 cx =
        resolution / 2U;

    const auto lowering =
        [&](const MaterialColumnPage& page,
            const u32 x)
        {
            return
                static_cast<f64>(
                    initial.At(x, y).
                        bedrockHeightMeters) -
                static_cast<f64>(
                    page.At(x, y).
                        bedrockHeightMeters);
        };

    const f64 glacierCenter =
        lowering(
            glacial.material,
            cx);

    const f64 glacierShoulders =
        0.5 *
        (lowering(
             glacial.material,
             cx - 1U) +
         lowering(
             glacial.material,
             cx + 1U));

    const f64 hydraulicCenter =
        lowering(
            hydraulic.material,
            cx);

    const f64 hydraulicShoulders =
        0.5 *
        (lowering(
             hydraulic.material,
             cx - 1U) +
         lowering(
             hydraulic.material,
             cx + 1U));

    const f64 glacialWideningRatio =
        glacierShoulders /
        std::max(
            glacierCenter,
            1.0e-6);

    const f64 hydraulicWideningRatio =
        hydraulicShoulders /
        std::max(
            hydraulicCenter,
            1.0e-6);

    Require(
        glacierShoulders > 0.05,
        "M15 glacier produced no measurable valley-side widening.");

    Require(
        glacialWideningRatio >
            hydraulicWideningRatio *
                1.35 +
            0.05,
        "M15 glacier morphology is not measurably wider/U-shaped relative "
        "to the hydraulic reference.");
}

void TestGlacialDebrisFeedsM14AndFormsMoraine()
{
    constexpr u32 resolution = 6U;

    auto geology =
        MakeGeology();

    auto page =
        MakeValley(
            resolution,
            6.0,
            terrain_geology::
                reference_rock::
                    VolcanicAsh);

    auto climate =
        ColdClimate(
            resolution,
            14.0F);

    auto config =
        TestConfig();

    config.iterations = 22U;
    config.basalErosionMetersPerYearAtReference = 0.15;
    config.lateralErosionFraction = 1.0;
    config.moraineThinIceThresholdMeters = 50.0;
    config.moraineStagnationSpeedMetersPerYear = 80.0;
    config.moraineDepositionRatePerYear = 1.5;

    const auto result =
        SimulateGlacialErosion(
            std::move(page),
            geology,
            climate,
            config);

    Require(
        result.sedimentExchange.
            has_value(),
        "M15 did not create the shared M14 sediment handoff.");

    const auto& accounting =
        result.sedimentExchange->
            Accounting();

    Require(
        accounting.
            physicalToMobile.
            coarseDebrisKg >
            0.0,
        "M15 glacial erosion did not publish coarse till/debris to M14.");

    Require(
        accounting.
            physicalToMobile.
            finesKg >
            0.0,
        "M15 glacial abrasion did not publish fines to M14.");

    Require(
        accounting.
            mobileToPhysical.
            TotalKg() >
            0.0,
        "M15 thin/stagnant ice did not deposit any moraine through M14.");

    f64 debrisDepth = 0.0;

    for (const auto& cell :
         result.material.Cells())
    {
        debrisDepth +=
            cell.debrisMeters;
    }

    Require(
        debrisDepth > 0.0,
        "M15 moraine deposition did not create physical M08 debris.");

    Require(
        result.materialBalance.
            balanceRelativeError <
            3.0e-5,
        "M15 glacial erosion/debris transport violated M08+M14 mass conservation.");
}

void TestDeterministicFixedInputs()
{
    constexpr u32 resolution = 5U;

    auto geology =
        MakeGeology();

    const auto page =
        MakeValley(
            resolution,
            7.0,
            terrain_geology::
                reference_rock::
                    VolcanicAsh);

    auto climate =
        ColdClimate(
            resolution,
            20.0F);

    auto config =
        TestConfig();

    config.iterations = 14U;

    const auto a =
        SimulateGlacialErosion(
            page,
            geology,
            climate,
            config);

    const auto b =
        SimulateGlacialErosion(
            page,
            geology,
            climate,
            config);

    Require(
        a.cells.size() ==
            b.cells.size(),
        "M15 deterministic runs produced different state sizes.");

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
                lhs.eligibility ==
                        rhs.eligibility &&
                    lhs.iceThicknessMeters ==
                        rhs.iceThicknessMeters &&
                    lhs.flowDx ==
                        rhs.flowDx &&
                    lhs.flowDy ==
                        rhs.flowDy &&
                    lhs.flowSpeedMetersPerYear ==
                        rhs.flowSpeedMetersPerYear &&
                    lhs.cumulativeErodedKg ==
                        rhs.cumulativeErodedKg &&
                    lhs.cumulativeMoraineDepositedKg ==
                        rhs.cumulativeMoraineDepositedKg,
                "M15 fixed glacier inputs are not deterministic.");

            const auto& lm =
                a.material.At(x, y);

            const auto& rm =
                b.material.At(x, y);

            Require(
                lm.bedrockHeightMeters ==
                        rm.bedrockHeightMeters &&
                    lm.regolithMeters ==
                        rm.regolithMeters &&
                    lm.soilMeters ==
                        rm.soilMeters &&
                    lm.sandMeters ==
                        rm.sandMeters &&
                    lm.debrisMeters ==
                        rm.debrisMeters,
                "M15 deterministic runs produced different M08 terrain state.");
        }
    }
}
} // namespace

int main()
{
    TestClimateMaskIsHardProcessBoundary();
    TestIceFlowConservesVolume();
    TestGlacierWidensValleyUnlikeHydraulicIncision();
    TestGlacialDebrisFeedsM14AndFormsMoraine();
    TestDeterministicFixedInputs();

    std::cout
        << "Orbit M15 glacial terrain process tests passed.\n";

    return EXIT_SUCCESS;
}
