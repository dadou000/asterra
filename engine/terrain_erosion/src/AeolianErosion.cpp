#include <orbit/terrain_erosion/AeolianErosion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace orbit::terrain_erosion
{
namespace
{
constexpr f64 kPi =
    3.14159265358979323846264338327950288;

struct WindStep
{
    i32 dx{0};
    i32 dy{0};
};

struct LocalExchange
{
    f64 depositSandKg{0.0};
    f64 depositFinesKg{0.0};

    f64 pickupSandDepthMeters{0.0};
    f64 pickupSoilDepthMeters{0.0};
    f64 abrasionDepthMeters{0.0};

    f64 reptationSandKg{0.0};
};

[[nodiscard]] std::size_t Index(
    const u32 resolution,
    const u32 x,
    const u32 y) noexcept
{
    return
        static_cast<std::size_t>(y) *
            resolution +
        x;
}

[[nodiscard]] bool Inside(
    const i32 x,
    const i32 y,
    const u32 resolution) noexcept
{
    return
        x >= 0 &&
        y >= 0 &&
        x <
            static_cast<i32>(resolution) &&
        y <
            static_cast<i32>(resolution);
}

[[nodiscard]] f64 WindSpeed(
    const AeolianCellForcing& forcing) noexcept
{
    return
        std::hypot(
            static_cast<f64>(
                forcing.windEastMetersPerSecond),
            static_cast<f64>(
                forcing.windNorthMetersPerSecond));
}

[[nodiscard]] WindStep QuantizeWind(
    const AeolianCellForcing& forcing) noexcept
{
    const f64 east =
        forcing.windEastMetersPerSecond;

    // Page-local y grows south, while positive windNorth points north.
    const f64 south =
        -static_cast<f64>(
            forcing.windNorthMetersPerSecond);

    if (east == 0.0 &&
        south == 0.0)
    {
        return {};
    }

    const f64 angle =
        std::atan2(
            south,
            east);

    const i32 sector =
        static_cast<i32>(
            std::llround(
                angle /
                (kPi / 4.0)));

    switch (
        (sector % 8 + 8) % 8)
    {
    case 0:
        return {1, 0};
    case 1:
        return {1, 1};
    case 2:
        return {0, 1};
    case 3:
        return {-1, 1};
    case 4:
        return {-1, 0};
    case 5:
        return {-1, -1};
    case 6:
        return {0, -1};
    case 7:
        return {1, -1};
    }

    return {};
}

[[nodiscard]] f64 SurfaceHeight(
    const terrain_material_column::MaterialColumnPage& page,
    const u32 x,
    const u32 y) noexcept
{
    return static_cast<f64>(
        page.At(
            x,
            y).
            SurfaceHeightMeters());
}

[[nodiscard]] f64 WindExposure(
    const terrain_material_column::MaterialColumnPage& page,
    const u32 x,
    const u32 y,
    const WindStep downwind,
    const AeolianErosionConfig& config) noexcept
{
    if (downwind.dx == 0 &&
        downwind.dy == 0)
    {
        return 0.0;
    }

    const f64 source =
        SurfaceHeight(
            page,
            x,
            y);

    f64 maximumObstructionSlope = 0.0;
    f64 windwardSlope = 0.0;

    for (u32 step = 1U;
         step <=
             config.shadowRayCells;
         ++step)
    {
        const i32 ux =
            static_cast<i32>(x) -
            downwind.dx *
                static_cast<i32>(step);

        const i32 uy =
            static_cast<i32>(y) -
            downwind.dy *
                static_cast<i32>(step);

        if (!Inside(
                ux,
                uy,
                page.Resolution()))
        {
            break;
        }

        const bool diagonal =
            downwind.dx != 0 &&
            downwind.dy != 0;

        const f64 distance =
            page.SpacingMeters() *
            static_cast<f64>(step) *
            (diagonal
                 ? 1.4142135623730951
                 : 1.0);

        const f64 upwind =
            SurfaceHeight(
                page,
                static_cast<u32>(ux),
                static_cast<u32>(uy));

        maximumObstructionSlope =
            std::max(
                maximumObstructionSlope,
                (upwind - source) /
                    distance);

        if (step == 1U)
        {
            windwardSlope =
                std::max(
                    (source - upwind) /
                        distance,
                    0.0);
        }
    }

    const f64 leeFactor =
        std::exp(
            -config.shadowStrength *
            std::max(
                maximumObstructionSlope,
                0.0));

    const f64 windwardFactor =
        1.0 +
        config.windwardExposureGain *
            windwardSlope;

    return
        std::clamp(
            leeFactor *
                windwardFactor,
            config.minimumExposure,
            config.maximumExposure);
}

[[nodiscard]] f64 DrynessResistance(
    const terrain_material_column::MaterialColumnCell& cell,
    const AeolianCellForcing& forcing,
    const AeolianErosionConfig& config) noexcept
{
    const f64 dryness =
        std::pow(
            std::max(
                1.0 -
                    static_cast<f64>(
                        cell.moisture),
                0.0),
            config.
                moistureSuppressionExponent);

    const f64 resistance =
        1.0 -
        std::clamp(
            static_cast<f64>(
                forcing.surfaceResistance),
            0.0,
            1.0);

    return
        dryness *
        resistance;
}

[[nodiscard]] f64 CarryingCapacityKgPerSquareMeter(
    const f64 speed,
    const f64 exposure,
    const f64 suppression,
    const AeolianErosionConfig& config) noexcept
{
    if (speed <= 0.0 ||
        exposure <= 0.0 ||
        suppression <= 0.0)
    {
        return 0.0;
    }

    return
        config.capacityCoefficient *
        std::pow(
            speed,
            config.windSpeedExponent) *
        exposure *
        suppression;
}
} // namespace

bool AeolianCellForcing::IsValid() const noexcept
{
    return
        std::isfinite(
            windEastMetersPerSecond) &&
        std::isfinite(
            windNorthMetersPerSecond) &&
        std::isfinite(
            surfaceResistance) &&
        surfaceResistance >= 0.0F &&
        surfaceResistance <= 1.0F;
}

bool AeolianErosionConfig::IsValid() const noexcept
{
    const auto nonnegative =
        [](const f64 value)
        {
            return
                std::isfinite(value) &&
                value >= 0.0;
        };

    const auto positive =
        [](const f64 value)
        {
            return
                std::isfinite(value) &&
                value > 0.0;
        };

    return
        iterations > 0U &&
        positive(timeStepSeconds) &&
        nonnegative(capacityCoefficient) &&
        nonnegative(windSpeedExponent) &&
        shadowRayCells > 0U &&
        nonnegative(shadowStrength) &&
        nonnegative(windwardExposureGain) &&
        positive(minimumExposure) &&
        positive(maximumExposure) &&
        minimumExposure <=
            maximumExposure &&
        nonnegative(pickupRatePerSecond) &&
        nonnegative(depositionRatePerSecond) &&
        std::isfinite(
            reptationFraction) &&
        reptationFraction >= 0.0 &&
        reptationFraction <= 1.0 &&
        nonnegative(
            saltationRatePerSecond) &&
        positive(
            referenceSaltationWindMetersPerSecond) &&
        nonnegative(
            maximumSandPickupDepthPerStepMeters) &&
        nonnegative(
            maximumSoilPickupDepthPerStepMeters) &&
        nonnegative(
            maximumDepositionDepthPerStepMeters) &&
        nonnegative(
            moistureSuppressionExponent) &&
        nonnegative(
            bedrockAbrasionMetersPerSecondAtReferenceWind) &&
        nonnegative(
            maximumBedrockAbrasionDepthPerStepMeters);
}

const AeolianCellState&
AeolianErosionResult::At(
    const u32 x,
    const u32 y) const
{
    if (x >= material.Resolution() ||
        y >= material.Resolution())
    {
        throw std::out_of_range(
            "Orbit M13 aeolian result coordinate is out of range.");
    }

    return
        cells[Index(
            material.Resolution(),
            x,
            y)];
}

AeolianErosionResult SimulateAeolianErosion(
    terrain_material_column::MaterialColumnPage material,
    const terrain_geology::GeologicalMaterialLibrary& geology,
    const std::span<const AeolianCellForcing> forcing,
    const AeolianErosionConfig& config)
{
    if (!config.IsValid())
    {
        throw std::invalid_argument(
            "Orbit M13 aeolian erosion configuration is invalid.");
    }

    const u32 resolution =
        material.Resolution();

    const std::size_t cellCount =
        static_cast<std::size_t>(
            resolution) *
        resolution;

    if (forcing.size() !=
        cellCount)
    {
        throw std::invalid_argument(
            "Orbit M13 wind forcing must match the physical page.");
    }

    for (const auto& cell :
         forcing)
    {
        if (!cell.IsValid())
        {
            throw std::invalid_argument(
                "Orbit M13 wind forcing contains an invalid cell.");
        }
    }

    const auto initialMass =
        material.QueryMass(
            geology);

    AeolianErosionResult result{
        .material = std::move(material),
        .cells =
            std::vector<AeolianCellState>(
                cellCount)
    };

    std::vector<WindStep> windStep(
        cellCount);

    std::vector<LocalExchange> exchange(
        cellCount);

    std::vector<f64> airborneSandAfterExchange(
        cellCount,
        0.0);

    std::vector<f64> airborneFinesAfterExchange(
        cellCount,
        0.0);

    std::vector<f64> nextAirborneSand(
        cellCount,
        0.0);

    std::vector<f64> nextAirborneFines(
        cellCount,
        0.0);

    std::vector<f64> reptationIncomingSandKg(
        cellCount,
        0.0);

    const f64 area =
        result.material.
            CellAreaSquareMeters();

    for (u32 iteration = 0U;
         iteration <
             config.iterations;
         ++iteration)
    {
        // 1. Resolve local exposure/capacity from the frozen physical surface.
        for (u32 y = 0U;
             y < resolution;
             ++y)
        {
            for (u32 x = 0U;
                 x < resolution;
                 ++x)
            {
                const std::size_t index =
                    Index(
                        resolution,
                        x,
                        y);

                const WindStep step =
                    QuantizeWind(
                        forcing[index]);

                windStep[index] =
                    step;

                const f64 exposure =
                    WindExposure(
                        result.material,
                        x,
                        y,
                        step,
                        config);

                const f64 suppression =
                    DrynessResistance(
                        result.material.At(
                            x,
                            y),
                        forcing[index],
                        config);

                const f64 capacity =
                    CarryingCapacityKgPerSquareMeter(
                        WindSpeed(
                            forcing[index]),
                        exposure,
                        suppression,
                        config);

                result.cells[index].
                    exposure =
                        static_cast<f32>(
                            exposure);

                result.cells[index].
                    capacityKgPerSquareMeter =
                        static_cast<f32>(
                            capacity);
            }
        }

        std::fill(
            exchange.begin(),
            exchange.end(),
            LocalExchange{});

        std::fill(
            reptationIncomingSandKg.begin(),
            reptationIncomingSandKg.end(),
            0.0);

        // 2. Capacity-controlled pickup/deposition. No physical mutation yet.
        for (u32 y = 0U;
             y < resolution;
             ++y)
        {
            for (u32 x = 0U;
                 x < resolution;
                 ++x)
            {
                const std::size_t index =
                    Index(
                        resolution,
                        x,
                        y);

                const auto& state =
                    result.cells[index];

                const auto& column =
                    result.material.At(
                        x,
                        y);

                const f64 capacityMass =
                    static_cast<f64>(
                        state.
                            capacityKgPerSquareMeter) *
                    area;

                const f64 airborneMass =
                    state.airborneSandKg +
                    state.airborneFinesKg;

                LocalExchange proposal{};

                if (airborneMass >
                    capacityMass)
                {
                    const f64 fraction =
                        std::clamp(
                            config.
                                depositionRatePerSecond *
                                config.
                                    timeStepSeconds,
                            0.0,
                            1.0);

                    const f64 desired =
                        (airborneMass -
                         capacityMass) *
                        fraction;

                    const f64 sandRatio =
                        airborneMass > 0.0
                            ? state.
                                  airborneSandKg /
                                  airborneMass
                            : 0.0;

                    const f64 finesRatio =
                        1.0 -
                        sandRatio;

                    const f64 maxSandMass =
                        config.
                            maximumDepositionDepthPerStepMeters *
                        area *
                        result.material.
                            Densities().
                            sandKgPerCubicMeter;

                    const f64 maxSoilMass =
                        config.
                            maximumDepositionDepthPerStepMeters *
                        area *
                        result.material.
                            Densities().
                            soilKgPerCubicMeter;

                    proposal.depositSandKg =
                        std::min(
                            desired *
                                sandRatio,
                            maxSandMass);

                    proposal.depositFinesKg =
                        std::min(
                            desired *
                                finesRatio,
                            maxSoilMass);
                }
                else if (
                    airborneMass <
                    capacityMass)
                {
                    const f64 fraction =
                        std::clamp(
                            config.
                                pickupRatePerSecond *
                                config.
                                    timeStepSeconds,
                            0.0,
                            1.0);

                    const f64 deficit =
                        (capacityMass -
                         airborneMass) *
                        fraction;

                    const auto exposed =
                        column.
                            ExposedSurface();

                    if (exposed ==
                        terrain_material_column::
                            ExposedSurfaceKind::
                                Sand)
                    {
                        const f64 density =
                            result.material.
                                Densities().
                                sandKgPerCubicMeter;

                        const f64 availableMass =
                            static_cast<f64>(
                                column.sandMeters) *
                            area *
                            density;

                        const f64 maximumMass =
                            config.
                                maximumSandPickupDepthPerStepMeters *
                            area *
                            density;

                        const f64 pickupMass =
                            std::min({
                                deficit,
                                availableMass,
                                maximumMass
                            });

                        proposal.
                            pickupSandDepthMeters =
                                pickupMass /
                                std::max(
                                    area *
                                        density,
                                    1.0e-12);

                        proposal.
                            reptationSandKg =
                                pickupMass *
                                config.
                                    reptationFraction;
                    }
                    else if (
                        exposed ==
                        terrain_material_column::
                            ExposedSurfaceKind::
                                Soil)
                    {
                        const f64 density =
                            result.material.
                                Densities().
                                soilKgPerCubicMeter;

                        const f64 availableMass =
                            static_cast<f64>(
                                column.soilMeters) *
                            area *
                            density;

                        const f64 maximumMass =
                            config.
                                maximumSoilPickupDepthPerStepMeters *
                            area *
                            density;

                        const f64 pickupMass =
                            std::min({
                                deficit,
                                availableMass,
                                maximumMass
                            });

                        proposal.
                            pickupSoilDepthMeters =
                                pickupMass /
                                std::max(
                                    area *
                                        density,
                                    1.0e-12);
                    }
                    else if (
                        exposed ==
                        terrain_material_column::
                            ExposedSurfaceKind::
                                Bedrock)
                    {
                        const auto* rock =
                            geology.Find(
                                column.
                                    bedrockMaterial);

                        if (rock == nullptr)
                        {
                            throw std::invalid_argument(
                                "Orbit M13 encountered unknown M02 bedrock.");
                        }

                        const f64 speed =
                            WindSpeed(
                                forcing[index]);

                        const f64 normalizedSpeed =
                            speed /
                            config.
                                referenceSaltationWindMetersPerSecond;

                        const f64 suppression =
                            DrynessResistance(
                                column,
                                forcing[index],
                                config);

                        const f64 abrasionDepth =
                            config.
                                bedrockAbrasionMetersPerSecondAtReferenceWind *
                            std::pow(
                                std::max(
                                    normalizedSpeed,
                                    0.0),
                                3.0) *
                            std::clamp(
                                static_cast<f64>(
                                    rock->
                                        aeolianErodibility),
                                0.0,
                                1.0) *
                            suppression *
                            static_cast<f64>(
                                state.exposure) *
                            config.
                                timeStepSeconds;

                        const f64 abrasionMass =
                            std::min(
                                abrasionDepth,
                                config.
                                    maximumBedrockAbrasionDepthPerStepMeters) *
                            area *
                            rock->density;

                        const f64 allowedMass =
                            std::min(
                                abrasionMass,
                                deficit);

                        proposal.
                            abrasionDepthMeters =
                                allowedMass /
                                std::max(
                                    area *
                                        static_cast<f64>(
                                            rock->density),
                                    1.0e-12);
                    }
                }

                exchange[index] =
                    proposal;
            }
        }

        // 3. Apply local physical exchange and construct post-exchange airborne
        // state. Reptation is accumulated separately into downwind receivers.
        for (u32 y = 0U;
             y < resolution;
             ++y)
        {
            for (u32 x = 0U;
                 x < resolution;
                 ++x)
            {
                const std::size_t index =
                    Index(
                        resolution,
                        x,
                        y);

                auto& state =
                    result.cells[index];

                const LocalExchange& proposal =
                    exchange[index];

                f64 sand =
                    state.airborneSandKg;

                f64 fines =
                    state.airborneFinesKg;

                if (proposal.depositSandKg >
                    0.0)
                {
                    const f64 depth =
                        proposal.
                            depositSandKg /
                        std::max(
                            area *
                                result.material.
                                    Densities().
                                    sandKgPerCubicMeter,
                            1.0e-12);

                    const f64 deposited =
                        result.material.Deposit(
                            x,
                            y,
                            terrain_material_column::
                                LooseMaterialKind::
                                    Sand,
                            depth);

                    sand =
                        std::max(
                            0.0,
                            sand -
                                deposited);

                    state.
                        cumulativeDepositedKg +=
                            deposited;
                }

                if (proposal.depositFinesKg >
                    0.0)
                {
                    const f64 depth =
                        proposal.
                            depositFinesKg /
                        std::max(
                            area *
                                result.material.
                                    Densities().
                                    soilKgPerCubicMeter,
                            1.0e-12);

                    const f64 deposited =
                        result.material.Deposit(
                            x,
                            y,
                            terrain_material_column::
                                LooseMaterialKind::
                                    Soil,
                            depth);

                    fines =
                        std::max(
                            0.0,
                            fines -
                                deposited);

                    state.
                        cumulativeDepositedKg +=
                            deposited;
                }

                if (proposal.
                        pickupSandDepthMeters >
                    0.0)
                {
                    const auto removal =
                        result.material.Erode(
                            x,
                            y,
                            proposal.
                                pickupSandDepthMeters,
                            geology);

                    const f64 reptated =
                        std::min(
                            proposal.
                                reptationSandKg,
                            removal.
                                removedMassKg);

                    sand +=
                        removal.
                            removedMassKg -
                        reptated;

                    state.
                        cumulativeSandPickedKg +=
                            removal.
                                removedMassKg;

                    state.
                        cumulativeReptatedKg +=
                            reptated;

                    const WindStep step =
                        windStep[index];

                    const i32 tx =
                        static_cast<i32>(x) +
                        step.dx;

                    const i32 ty =
                        static_cast<i32>(y) +
                        step.dy;

                    const std::size_t target =
                        Inside(
                            tx,
                            ty,
                            resolution)
                            ? Index(
                                  resolution,
                                  static_cast<u32>(tx),
                                  static_cast<u32>(ty))
                            : index;

                    reptationIncomingSandKg[target] +=
                        reptated;
                }

                if (proposal.
                        pickupSoilDepthMeters >
                    0.0)
                {
                    const auto removal =
                        result.material.Erode(
                            x,
                            y,
                            proposal.
                                pickupSoilDepthMeters,
                            geology);

                    fines +=
                        removal.
                            removedMassKg;

                    state.
                        cumulativeSoilPickedKg +=
                            removal.
                                removedMassKg;
                }

                if (proposal.
                        abrasionDepthMeters >
                    0.0)
                {
                    const auto removal =
                        result.material.Erode(
                            x,
                            y,
                            proposal.
                                abrasionDepthMeters,
                            geology);

                    sand +=
                        removal.
                            removedMassKg;

                    state.
                        cumulativeBedrockAbradedKg +=
                            removal.
                                removedMassKg;
                }

                airborneSandAfterExchange[index] =
                    sand;

                airborneFinesAfterExchange[index] =
                    fines;
            }
        }

        for (u32 y = 0U;
             y < resolution;
             ++y)
        {
            for (u32 x = 0U;
                 x < resolution;
                 ++x)
            {
                const std::size_t index =
                    Index(
                        resolution,
                        x,
                        y);

                const f64 reptated =
                    reptationIncomingSandKg[index];

                if (reptated <= 0.0)
                {
                    continue;
                }

                const f64 depth =
                    reptated /
                    std::max(
                        area *
                            result.material.
                                Densities().
                                sandKgPerCubicMeter,
                        1.0e-12);

                static_cast<void>(
                    result.material.Deposit(
                        x,
                        y,
                        terrain_material_column::
                            LooseMaterialKind::
                                Sand,
                        depth));
            }
        }

        // 4. Saltation advection. Closed M13 page boundaries retain airborne
        // mass in the edge cell; M14 later owns explicit boundary flux.
        std::fill(
            nextAirborneSand.begin(),
            nextAirborneSand.end(),
            0.0);

        std::fill(
            nextAirborneFines.begin(),
            nextAirborneFines.end(),
            0.0);

        for (u32 y = 0U;
             y < resolution;
             ++y)
        {
            for (u32 x = 0U;
                 x < resolution;
                 ++x)
            {
                const std::size_t index =
                    Index(
                        resolution,
                        x,
                        y);

                const f64 speed =
                    WindSpeed(
                        forcing[index]);

                const f64 transportedFraction =
                    std::clamp(
                        config.
                            saltationRatePerSecond *
                        config.
                            timeStepSeconds *
                        speed /
                        config.
                            referenceSaltationWindMetersPerSecond,
                        0.0,
                        1.0);

                const WindStep step =
                    windStep[index];

                const i32 tx =
                    static_cast<i32>(x) +
                    step.dx;

                const i32 ty =
                    static_cast<i32>(y) +
                    step.dy;

                const std::size_t target =
                    Inside(
                        tx,
                        ty,
                        resolution)
                        ? Index(
                              resolution,
                              static_cast<u32>(tx),
                              static_cast<u32>(ty))
                        : index;

                const f64 outgoingSand =
                    airborneSandAfterExchange[index] *
                    transportedFraction;

                const f64 outgoingFines =
                    airborneFinesAfterExchange[index] *
                    transportedFraction;

                nextAirborneSand[index] +=
                    airborneSandAfterExchange[index] -
                    outgoingSand;

                nextAirborneFines[index] +=
                    airborneFinesAfterExchange[index] -
                    outgoingFines;

                nextAirborneSand[target] +=
                    outgoingSand;

                nextAirborneFines[target] +=
                    outgoingFines;
            }
        }

        for (std::size_t index = 0U;
             index < cellCount;
             ++index)
        {
            result.cells[index].
                airborneSandKg =
                    nextAirborneSand[index];

            result.cells[index].
                airborneFinesKg =
                    nextAirborneFines[index];
        }
    }

    const auto finalMass =
        result.material.QueryMass(
            geology);

    f64 airborneMass = 0.0;

    for (const auto& state :
         result.cells)
    {
        airborneMass +=
            state.airborneSandKg +
            state.airborneFinesKg;
    }

    const f64 abradedBedrock =
        std::max(
            finalMass.
                excavatedBedrockKg -
                initialMass.
                    excavatedBedrockKg,
            0.0);

    const f64 error =
        initialMass.LooseMassKg() +
        abradedBedrock -
        finalMass.LooseMassKg() -
        airborneMass;

    const f64 reference =
        std::max(
            initialMass.LooseMassKg() +
                abradedBedrock,
            1.0);

    result.massBalance = {
        .initialLooseMassKg =
            initialMass.LooseMassKg(),
        .finalLooseMassKg =
            finalMass.LooseMassKg(),
        .abradedBedrockMassKg =
            abradedBedrock,
        .finalAirborneMassKg =
            airborneMass,
        .boundaryLossKg = 0.0,
        .materialBalanceErrorKg =
            error,
        .materialBalanceRelativeError =
            std::abs(error) /
            reference
    };

    return result;
}
} // namespace orbit::terrain_erosion
