#include <orbit/terrain_erosion/ThermalErosion.hpp>

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
        << "M12 failure: "
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
        .name = "M12 competent basalt",
        .hardness = 0.98F,
        .cohesion = 0.94F,
        .hydraulicErodibility = 0.08F,
        .aeolianErodibility = 0.02F,
        .permeability = 0.08F,
        .chemicalWeatherability = 0.15F,
        .fractureTendency = 0.22F,
        .density = 3'050.0F
    });

    geology.Upsert({
        .id =
            terrain_geology::reference_rock::VolcanicAsh,
        .name = "M12 fractured ash",
        .hardness = 0.08F,
        .cohesion = 0.05F,
        .hydraulicErodibility = 0.98F,
        .aeolianErodibility = 0.85F,
        .permeability = 0.75F,
        .chemicalWeatherability = 0.85F,
        .fractureTendency = 0.92F,
        .density = 1'650.0F
    });

    return geology;
}

MaterialColumnCell Cell(
    const f32 bedrockHeight,
    const terrain_geology::RockTypeId rock)
{
    return {
        .bedrockHeightMeters =
            bedrockHeight,
        .referenceBedrockHeightMeters =
            bedrockHeight,
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

MaterialColumnPage LoosePeak(
    const LooseMaterialKind kind,
    const f32 depthMeters)
{
    constexpr u32 resolution = 5U;

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
            auto cell =
                Cell(
                    (x == 2U &&
                     y == 2U)
                        ? 3.0F
                        : 0.0F,
                    terrain_geology::
                        reference_rock::Basalt);

            switch (kind)
            {
            case LooseMaterialKind::Regolith:
                cell.regolithMeters =
                    depthMeters;
                break;
            case LooseMaterialKind::Soil:
                cell.soilMeters =
                    depthMeters;
                break;
            case LooseMaterialKind::Sand:
                cell.sandMeters =
                    depthMeters;
                break;
            case LooseMaterialKind::Debris:
                cell.debrisMeters =
                    depthMeters;
                break;
            }

            page.SetCell(
                x,
                y,
                cell);
        }
    }

    return page;
}

f64 TotalMovedOut(
    const ThermalErosionResult& result)
{
    f64 value = 0.0;

    for (const auto& cell :
         result.cells)
    {
        value +=
            cell.movedOutKg;
    }

    return value;
}

void TestMaterialSpecificRepose()
{
    auto geology =
        MakeGeology();

    ThermalErosionConfig config{};
    config.maximumIterations = 1U;
    config.bedrockFractureRate = 0.0;
    config.maximumTransferDepthPerIterationMeters =
        1.0;

    const auto sand =
        SimulateThermalErosion(
            LoosePeak(
                LooseMaterialKind::Sand,
                1.0F),
            geology,
            {},
            config);

    const auto debris =
        SimulateThermalErosion(
            LoosePeak(
                LooseMaterialKind::Debris,
                1.0F),
            geology,
            {},
            config);

    const auto soil =
        SimulateThermalErosion(
            LoosePeak(
                LooseMaterialKind::Soil,
                1.0F),
            geology,
            {},
            config);

    const f64 sandRemaining =
        sand.material.At(2U, 2U).
            sandMeters;

    const f64 debrisRemaining =
        debris.material.At(2U, 2U).
            debrisMeters;

    const f64 soilRemaining =
        soil.material.At(2U, 2U).
            soilMeters;

    Require(
        sandRemaining <
            debrisRemaining &&
        debrisRemaining <
            soilRemaining,
        "M12 sand, debris and cohesive soil do not exhibit distinct "
        "angle-of-repose behavior.");

    RequireNear(
        sand.massBalance.
            materialBalanceRelativeError,
        0.0,
        2.0e-6,
        "M12 loose-material redistribution did not conserve mass.");
}

void TestLooseAvalancheDoesNotSmoothBedrock()
{
    auto geology =
        MakeGeology();

    auto page =
        LoosePeak(
            LooseMaterialKind::Sand,
            1.0F);

    std::vector<f32> originalBedrock;

    for (const auto& cell :
         page.Cells())
    {
        originalBedrock.push_back(
            cell.bedrockHeightMeters);
    }

    ThermalErosionConfig config{};
    config.maximumIterations = 16U;
    config.bedrockFractureRate = 0.0;

    const auto result =
        SimulateThermalErosion(
            std::move(page),
            geology,
            {},
            config);

    std::size_t index = 0U;

    for (const auto& cell :
         result.material.Cells())
    {
        Require(
            cell.bedrockHeightMeters ==
                originalBedrock[index],
            "M12 loose-slope relaxation smoothed bedrock indiscriminately.");

        ++index;
    }

    Require(
        TotalMovedOut(result) > 0.0,
        "M12 loose slope did not avalanche.");
}

void TestWeakBedrockProducesTalus()
{
    constexpr u32 resolution = 3U;

    auto geology =
        MakeGeology();

    const auto makeRockPeak =
        [&](const terrain_geology::RockTypeId rock)
        {
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
                            rock));
                }
            }

            return page;
        };

    ThermalErosionConfig config{};
    config.maximumIterations = 10U;
    config.maximumTransferDepthPerIterationMeters =
        0.50;
    config.bedrockFractureRate =
        0.75;

    const auto hard =
        SimulateThermalErosion(
            makeRockPeak(
                terrain_geology::
                    reference_rock::Basalt),
            geology,
            {},
            config);

    const auto weak =
        SimulateThermalErosion(
            makeRockPeak(
                terrain_geology::
                    reference_rock::VolcanicAsh),
            geology,
            {},
            config);

    Require(
        weak.massBalance.
            fracturedBedrockMassKg >
        hard.massBalance.
            fracturedBedrockMassKg *
                2.0 +
            1.0e-6,
        "M02 weak/fractured rock did not fail more readily than competent basalt.");

    f64 weakDebris = 0.0;

    for (const auto& cell :
         weak.material.Cells())
    {
        weakDebris +=
            static_cast<f64>(
                cell.debrisMeters) *
            weak.material.
                CellAreaSquareMeters() *
            weak.material.
                Densities().
                debrisKgPerCubicMeter;
    }

    Require(
        weakDebris > 0.0,
        "M12 bedrock failure did not create talus/debris.");

    Require(
        weak.massBalance.
            materialBalanceRelativeError <
            3.0e-6,
        "M12 bedrock-to-debris conversion violated mass conservation.");
}

void TestProtectionPreventsSourceFailure()
{
    constexpr u32 resolution = 5U;

    auto geology =
        MakeGeology();

    auto unprotectedPage =
        LoosePeak(
            LooseMaterialKind::Sand,
            1.0F);

    auto protectedPage =
        unprotectedPage;

    std::vector<f32> protection(
        static_cast<std::size_t>(
            resolution) *
            resolution,
        0.0F);

    protection[
        2U * resolution +
        2U] =
        1.0F;

    ThermalErosionConfig config{};
    config.maximumIterations = 1U;
    config.bedrockFractureRate = 0.0;

    const auto unprotected =
        SimulateThermalErosion(
            std::move(
                unprotectedPage),
            geology,
            {},
            config);

    const auto protectedResult =
        SimulateThermalErosion(
            std::move(
                protectedPage),
            geology,
            protection,
            config);

    Require(
        unprotected.At(2U, 2U).
            movedOutKg >
            0.0,
        "M12 baseline source was not unstable.");

    RequireNear(
        protectedResult.At(2U, 2U).
            movedOutKg,
        0.0,
        1.0e-12,
        "Full M04 protection did not preserve the thermal source cell.");
}

void TestLocalConvergenceAndDeterminism()
{
    auto geology =
        MakeGeology();

    const auto page =
        LoosePeak(
            LooseMaterialKind::Sand,
            2.0F);

    ThermalErosionConfig config{};
    config.maximumIterations = 128U;
    config.bedrockFractureRate = 0.0;
    config.maximumTransferDepthPerIterationMeters =
        0.20;
    config.convergenceDepthMeters =
        2.0e-5;

    const auto a =
        SimulateThermalErosion(
            page,
            geology,
            {},
            config);

    const auto b =
        SimulateThermalErosion(
            page,
            geology,
            {},
            config);

    Require(
        a.converged,
        "M12 local relaxation did not converge within its bounded iteration budget.");

    Require(
        a.iterationsExecuted <=
            config.maximumIterations,
        "M12 exceeded its local convergence iteration budget.");

    Require(
        a.cells.size() ==
            b.cells.size(),
        "M12 deterministic runs produced different state sizes.");

    for (u32 y = 0U;
         y < page.Resolution();
         ++y)
    {
        for (u32 x = 0U;
             x < page.Resolution();
             ++x)
        {
            const auto& lhs =
                a.material.At(
                    x,
                    y);

            const auto& rhs =
                b.material.At(
                    x,
                    y);

            Require(
                lhs.bedrockHeightMeters ==
                        rhs.bedrockHeightMeters &&
                    lhs.regolithMeters ==
                        rhs.regolithMeters &&
                    lhs.soilMeters ==
                        rhs.soilMeters &&
                    lhs.sandMeters ==
                        rhs.sandMeters &&
                    lhs.debrisMeters ==
                        rhs.debrisMeters,
                "M12 fixed inputs are not deterministic.");
        }
    }
}
} // namespace

int main()
{
    TestMaterialSpecificRepose();
    TestLooseAvalancheDoesNotSmoothBedrock();
    TestWeakBedrockProducesTalus();
    TestProtectionPreventsSourceFailure();
    TestLocalConvergenceAndDeterminism();

    std::cout
        << "Orbit M12 thermal erosion tests passed.\n";

    return EXIT_SUCCESS;
}
