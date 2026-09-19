#include <orbit/terrain_erosion/HydraulicErosion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace orbit::terrain_erosion
{
namespace
{
enum class Direction : u8
{
    West,
    East,
    North,
    South
};

struct Flux4
{
    f64 west{0.0};
    f64 east{0.0};
    f64 north{0.0};
    f64 south{0.0};

    [[nodiscard]] f64 Sum() const noexcept
    {
        return west + east + north + south;
    }
};

[[nodiscard]] std::size_t Index(
    const u32 resolution,
    const u32 x,
    const u32 y) noexcept
{
    return
        static_cast<std::size_t>(y) *
            static_cast<std::size_t>(resolution) +
        static_cast<std::size_t>(x);
}

[[nodiscard]] bool Inside(
    const i32 coordinate,
    const u32 resolution) noexcept
{
    return
        coordinate >= 0 &&
        coordinate <
            static_cast<i32>(resolution);
}

[[nodiscard]] f64 SurfaceHeight(
    const terrain_material_column::MaterialColumnPage& material,
    const u32 x,
    const u32 y) noexcept
{
    return static_cast<f64>(
        material.At(x, y).SurfaceHeightMeters());
}

[[nodiscard]] f64 Head(
    const terrain_material_column::MaterialColumnPage& material,
    const std::vector<HydraulicCellState>& cells,
    const u32 resolution,
    const u32 x,
    const u32 y) noexcept
{
    return
        SurfaceHeight(material, x, y) +
        cells[Index(resolution, x, y)].
            waterDepthMeters;
}

[[nodiscard]] f64 ExposedDensity(
    const terrain_material_column::MaterialColumnPage& material,
    const terrain_geology::GeologicalMaterialLibrary& geology,
    const u32 x,
    const u32 y)
{
    const auto& cell =
        material.At(x, y);

    switch (cell.ExposedSurface())
    {
    case terrain_material_column::ExposedSurfaceKind::Regolith:
        return material.Densities().regolithKgPerCubicMeter;
    case terrain_material_column::ExposedSurfaceKind::Soil:
        return material.Densities().soilKgPerCubicMeter;
    case terrain_material_column::ExposedSurfaceKind::Sand:
        return material.Densities().sandKgPerCubicMeter;
    case terrain_material_column::ExposedSurfaceKind::Debris:
        return material.Densities().debrisKgPerCubicMeter;
    case terrain_material_column::ExposedSurfaceKind::Bedrock:
    {
        const auto* rock =
            geology.Find(cell.bedrockMaterial);

        if (rock == nullptr)
        {
            throw std::invalid_argument(
                "Orbit M11 material page references an unknown M02 rock.");
        }

        return rock->density;
    }
    }

    return material.Densities().regolithKgPerCubicMeter;
}

[[nodiscard]] f64 ExposedMobility(
    const terrain_material_column::MaterialColumnPage& material,
    const terrain_geology::GeologicalMaterialLibrary& geology,
    const HydraulicErosionConfig& config,
    const u32 x,
    const u32 y)
{
    const auto& cell =
        material.At(x, y);

    switch (cell.ExposedSurface())
    {
    case terrain_material_column::ExposedSurfaceKind::Regolith:
        return config.regolithMobility;
    case terrain_material_column::ExposedSurfaceKind::Soil:
        return config.soilMobility;
    case terrain_material_column::ExposedSurfaceKind::Sand:
        return config.sandMobility;
    case terrain_material_column::ExposedSurfaceKind::Debris:
        return config.debrisMobility;
    case terrain_material_column::ExposedSurfaceKind::Bedrock:
    {
        const auto* rock =
            geology.Find(cell.bedrockMaterial);

        if (rock == nullptr)
        {
            throw std::invalid_argument(
                "Orbit M11 material page references an unknown M02 rock.");
        }

        const f64 hydraulic =
            std::clamp(
                static_cast<f64>(
                    rock->hydraulicErodibility),
                0.0,
                1.0);

        const f64 hardness =
            std::clamp(
                static_cast<f64>(
                    rock->hardness),
                0.0,
                1.0);

        const f64 cohesion =
            std::clamp(
                static_cast<f64>(
                    rock->cohesion),
                0.0,
                1.0);

        return
            hydraulic *
            (1.0 - 0.65 * hardness) *
            (1.0 - 0.35 * cohesion);
    }
    }

    return 0.0;
}

[[nodiscard]] f64 Permeability(
    const terrain_material_column::MaterialColumnPage& material,
    const terrain_geology::GeologicalMaterialLibrary& geology,
    const u32 x,
    const u32 y)
{
    const auto& cell =
        material.At(x, y);

    const auto* rock =
        geology.Find(cell.bedrockMaterial);

    if (rock == nullptr)
    {
        throw std::invalid_argument(
            "Orbit M11 material page references an unknown M02 rock.");
    }

    return
        std::clamp(
            static_cast<f64>(
                rock->permeability),
            0.0,
            1.0);
}

[[nodiscard]] f64 MaximumDownhillSlope(
    const terrain_material_column::MaterialColumnPage& material,
    const u32 x,
    const u32 y) noexcept
{
    const u32 resolution =
        material.Resolution();

    const f64 source =
        SurfaceHeight(material, x, y);

    f64 maximumSlope = 0.0;

    constexpr std::array<std::pair<i32, i32>, 4> offsets{{
        {-1, 0},
        {1, 0},
        {0, -1},
        {0, 1}
    }};

    for (const auto [dx, dy] : offsets)
    {
        const i32 nx =
            static_cast<i32>(x) + dx;

        const i32 ny =
            static_cast<i32>(y) + dy;

        if (!Inside(nx, resolution) ||
            !Inside(ny, resolution))
        {
            continue;
        }

        const f64 downstream =
            SurfaceHeight(
                material,
                static_cast<u32>(nx),
                static_cast<u32>(ny));

        maximumSlope =
            std::max(
                maximumSlope,
                (source - downstream) /
                    material.SpacingMeters());
    }

    return std::max(maximumSlope, 0.0);
}

[[nodiscard]] f64 FluxToward(
    const Flux4& flux,
    const Direction direction) noexcept
{
    switch (direction)
    {
    case Direction::West:
        return flux.west;
    case Direction::East:
        return flux.east;
    case Direction::North:
        return flux.north;
    case Direction::South:
        return flux.south;
    }

    return 0.0;
}

[[nodiscard]] SedimentMass ScaleSediment(
    const SedimentMass& mass,
    const f64 scale) noexcept
{
    return {
        .sandKg = mass.sandKg * scale,
        .finesKg = mass.finesKg * scale,
        .coarseDebrisKg = mass.coarseDebrisKg * scale
    };
}
} // namespace

bool HydraulicErosionConfig::IsValid() const noexcept
{
    return
        iterations > 0U &&
        std::isfinite(timeStepSeconds) &&
        timeStepSeconds > 0.0 &&
        std::isfinite(rainfallMetersPerSecond) &&
        rainfallMetersPerSecond >= 0.0 &&
        std::isfinite(gravityMetersPerSecondSquared) &&
        gravityMetersPerSecondSquared > 0.0 &&
        std::isfinite(pipeCrossSectionSquareMeters) &&
        pipeCrossSectionSquareMeters > 0.0 &&
        std::isfinite(sedimentCapacityCoefficient) &&
        sedimentCapacityCoefficient >= 0.0 &&
        std::isfinite(
            maximumSedimentConcentrationKgPerCubicMeter) &&
        maximumSedimentConcentrationKgPerCubicMeter >= 0.0 &&
        std::isfinite(erosionRatePerSecond) &&
        erosionRatePerSecond >= 0.0 &&
        std::isfinite(depositionRatePerSecond) &&
        depositionRatePerSecond >= 0.0 &&
        std::isfinite(maximumErosionDepthPerStepMeters) &&
        maximumErosionDepthPerStepMeters >= 0.0 &&
        std::isfinite(maximumDepositionDepthPerStepMeters) &&
        maximumDepositionDepthPerStepMeters >= 0.0 &&
        std::isfinite(regolithMobility) &&
        regolithMobility >= 0.0 &&
        regolithMobility <= 1.0 &&
        std::isfinite(soilMobility) &&
        soilMobility >= 0.0 &&
        soilMobility <= 1.0 &&
        std::isfinite(sandMobility) &&
        sandMobility >= 0.0 &&
        sandMobility <= 1.0 &&
        std::isfinite(debrisMobility) &&
        debrisMobility >= 0.0 &&
        debrisMobility <= 1.0 &&
        std::isfinite(infiltrationMetersPerSecond) &&
        infiltrationMetersPerSecond >= 0.0 &&
        std::isfinite(moistureCapacityDepthMeters) &&
        moistureCapacityDepthMeters > 0.0 &&
        std::isfinite(evaporationRatePerSecond) &&
        evaporationRatePerSecond >= 0.0;
}

const HydraulicCellState&
HydraulicErosionResult::At(
    const u32 x,
    const u32 y) const
{
    if (x >= material.Resolution() ||
        y >= material.Resolution())
    {
        throw std::out_of_range(
            "Orbit M11 hydraulic result coordinate is out of range.");
    }

    return cells[Index(
        material.Resolution(),
        x,
        y)];
}

HydraulicCellState&
HydraulicErosionResult::At(
    const u32 x,
    const u32 y)
{
    if (x >= material.Resolution() ||
        y >= material.Resolution())
    {
        throw std::out_of_range(
            "Orbit M11 hydraulic result coordinate is out of range.");
    }

    return cells[Index(
        material.Resolution(),
        x,
        y)];
}

HydraulicErosionResult SimulateHydraulicErosion(
    terrain_material_column::MaterialColumnPage material,
    const terrain_geology::GeologicalMaterialLibrary& geology,
    const std::span<const f32> rainfallRateMetersPerSecond,
    const HydraulicErosionConfig& config)
{
    if (!config.IsValid())
    {
        throw std::invalid_argument(
            "Orbit M11 hydraulic erosion configuration is invalid.");
    }

    const u32 resolution =
        material.Resolution();

    const std::size_t cellCount =
        static_cast<std::size_t>(resolution) *
        resolution;

    if (!rainfallRateMetersPerSecond.empty() &&
        rainfallRateMetersPerSecond.size() != cellCount)
    {
        throw std::invalid_argument(
            "Orbit M11 rainfall source field must match the physical page.");
    }

    const auto initialMass =
        material.QueryMass(geology);

    HydraulicErosionResult result{
        .material = std::move(material),
        .cells =
            std::vector<HydraulicCellState>(
                cellCount)
    };

    std::vector<Flux4> nextFlux(
        cellCount);

    std::vector<f64> waterBeforeFlow(
        cellCount,
        0.0);

    std::vector<f64> waterAfterFlow(
        cellCount,
        0.0);

    std::vector<math::Double2> nextVelocity(
        cellCount);

    std::vector<SedimentMass> sedimentAfterExchange(
        cellCount);

    std::vector<SedimentMass> sedimentNext(
        cellCount);

    result.sedimentExchange.emplace(
        resolution,
        material.SpacingMeters());

    const f64 spacing =
        result.material.SpacingMeters();

    const f64 area =
        result.material.CellAreaSquareMeters();

    const f64 dt =
        config.timeStepSeconds;

    for (u32 iteration = 0U;
         iteration < config.iterations;
         ++iteration)
    {
        // 1. Rain/water sources.
        for (u32 y = 0; y < resolution; ++y)
        {
            for (u32 x = 0; x < resolution; ++x)
            {
                const std::size_t index =
                    Index(
                        resolution,
                        x,
                        y);

                const f64 localRain =
                    rainfallRateMetersPerSecond.empty()
                        ? 0.0
                        : std::max(
                            static_cast<f64>(
                                rainfallRateMetersPerSecond[index]),
                            0.0);

                HydraulicCellState& state =
                    result.cells[index];

                state.waterDepthMeters +=
                    (config.rainfallMetersPerSecond +
                     localRain) *
                    dt;

                waterBeforeFlow[index] =
                    state.waterDepthMeters;
            }
        }

        // 2. Virtual-pipe outflow. Each source scales all four pipes together
        // so no step can export more water than the cell currently contains.
        for (u32 y = 0; y < resolution; ++y)
        {
            for (u32 x = 0; x < resolution; ++x)
            {
                const std::size_t index =
                    Index(
                        resolution,
                        x,
                        y);

                const HydraulicCellState& previous =
                    result.cells[index];

                Flux4 flux{
                    .west =
                        previous.
                            fluxWestCubicMetersPerSecond,
                    .east =
                        previous.
                            fluxEastCubicMetersPerSecond,
                    .north =
                        previous.
                            fluxNorthCubicMetersPerSecond,
                    .south =
                        previous.
                            fluxSouthCubicMetersPerSecond
                };

                const f64 sourceHead =
                    Head(
                        result.material,
                        result.cells,
                        resolution,
                        x,
                        y);

                const auto update =
                    [&](const i32 nx,
                        const i32 ny,
                        const f64 oldFlux)
                    {
                        if (!Inside(nx, resolution) ||
                            !Inside(ny, resolution))
                        {
                            return 0.0;
                        }

                        const f64 neighborHead =
                            Head(
                                result.material,
                                result.cells,
                                resolution,
                                static_cast<u32>(nx),
                                static_cast<u32>(ny));

                        const f64 headDifference =
                            sourceHead -
                            neighborHead;

                        return std::max(
                            0.0,
                            oldFlux +
                                dt *
                                config.
                                    pipeCrossSectionSquareMeters *
                                config.
                                    gravityMetersPerSecondSquared *
                                headDifference /
                                spacing);
                    };

                flux.west =
                    update(
                        static_cast<i32>(x) - 1,
                        static_cast<i32>(y),
                        flux.west);

                flux.east =
                    update(
                        static_cast<i32>(x) + 1,
                        static_cast<i32>(y),
                        flux.east);

                flux.north =
                    update(
                        static_cast<i32>(x),
                        static_cast<i32>(y) - 1,
                        flux.north);

                flux.south =
                    update(
                        static_cast<i32>(x),
                        static_cast<i32>(y) + 1,
                        flux.south);

                const f64 totalFlux =
                    flux.Sum();

                if (totalFlux > 0.0)
                {
                    const f64 waterVolume =
                        waterBeforeFlow[index] *
                        area;

                    const f64 scale =
                        std::min(
                            1.0,
                            waterVolume /
                                (totalFlux * dt));

                    flux.west *= scale;
                    flux.east *= scale;
                    flux.north *= scale;
                    flux.south *= scale;
                }

                nextFlux[index] = flux;
            }
        }

        // 3. Water continuity and velocity field.
        for (u32 y = 0; y < resolution; ++y)
        {
            for (u32 x = 0; x < resolution; ++x)
            {
                const std::size_t index =
                    Index(
                        resolution,
                        x,
                        y);

                const Flux4& own =
                    nextFlux[index];

                f64 inWest = 0.0;
                f64 inEast = 0.0;
                f64 inNorth = 0.0;
                f64 inSouth = 0.0;

                if (x > 0U)
                {
                    inWest =
                        nextFlux[Index(
                            resolution,
                            x - 1U,
                            y)].
                            east;
                }

                if (x + 1U < resolution)
                {
                    inEast =
                        nextFlux[Index(
                            resolution,
                            x + 1U,
                            y)].
                            west;
                }

                if (y > 0U)
                {
                    inNorth =
                        nextFlux[Index(
                            resolution,
                            x,
                            y - 1U)].
                            south;
                }

                if (y + 1U < resolution)
                {
                    inSouth =
                        nextFlux[Index(
                            resolution,
                            x,
                            y + 1U)].
                            north;
                }

                const f64 incoming =
                    inWest +
                    inEast +
                    inNorth +
                    inSouth;

                const f64 outgoing =
                    own.Sum();

                const f64 nextVolume =
                    std::max(
                        0.0,
                        waterBeforeFlow[index] *
                            area +
                        dt *
                            (incoming - outgoing));

                const f64 nextDepth =
                    nextVolume /
                    area;

                waterAfterFlow[index] =
                    nextDepth;

                const f64 crossSection =
                    std::max(
                        nextDepth *
                            spacing,
                        1.0e-9);

                const f64 qx =
                    0.5 *
                    ((own.east + inWest) -
                     (own.west + inEast));

                const f64 qy =
                    0.5 *
                    ((own.south + inNorth) -
                     (own.north + inSouth));

                nextVelocity[index] = {
                    qx / crossSection,
                    qy / crossSection
                };
            }
        }

        // Commit the hydrodynamic state before erosion/deposition.
        for (std::size_t index = 0;
             index < cellCount;
             ++index)
        {
            HydraulicCellState& state =
                result.cells[index];

            const Flux4& flux =
                nextFlux[index];

            state.waterDepthMeters =
                waterAfterFlow[index];

            state.fluxWestCubicMetersPerSecond =
                flux.west;

            state.fluxEastCubicMetersPerSecond =
                flux.east;

            state.fluxNorthCubicMetersPerSecond =
                flux.north;

            state.fluxSouthCubicMetersPerSecond =
                flux.south;

            state.velocityMetersPerSecond =
                nextVelocity[index];
        }

        // 4. Capacity-controlled erosion/deposition against the actual M08
        // layered column. Material exchange is recorded in kg.
        for (u32 y = 0; y < resolution; ++y)
        {
            for (u32 x = 0; x < resolution; ++x)
            {
                const std::size_t index =
                    Index(
                        resolution,
                        x,
                        y);

                HydraulicCellState& state =
                    result.cells[index];

                const f64 speed =
                    std::sqrt(
                        state.
                            velocityMetersPerSecond.x *
                        state.
                            velocityMetersPerSecond.x +
                        state.
                            velocityMetersPerSecond.y *
                        state.
                            velocityMetersPerSecond.y);

                const f64 slope =
                    MaximumDownhillSlope(
                        result.material,
                        x,
                        y);

                const f64 waterVolume =
                    state.waterDepthMeters *
                    area;

                const f64 concentration =
                    std::clamp(
                        config.
                            sedimentCapacityCoefficient *
                            speed *
                            slope,
                        0.0,
                        config.
                            maximumSedimentConcentrationKgPerCubicMeter);

                const f64 capacityKg =
                    concentration *
                    waterVolume;

                auto& shared =
                    result.sedimentExchange->
                        At(x, y).
                        waterborne;

                state.suspendedSediment =
                    shared;

                state.suspendedSedimentKg =
                    shared.TotalKg();

                if (state.suspendedSedimentKg <
                        capacityKg &&
                    config.maximumErosionDepthPerStepMeters >
                        0.0)
                {
                    const f64 deficit =
                        capacityKg -
                        state.
                            suspendedSedimentKg;

                    const f64 exchangeFraction =
                        std::clamp(
                            config.
                                erosionRatePerSecond *
                                dt,
                            0.0,
                            1.0);

                    const f64 mobility =
                        ExposedMobility(
                            result.material,
                            geology,
                            config,
                            x,
                            y);

                    const f64 density =
                        ExposedDensity(
                            result.material,
                            geology,
                            x,
                            y);

                    const f64 desiredMass =
                        deficit *
                        exchangeFraction *
                        mobility;

                    const f64 desiredDepth =
                        std::min(
                            desiredMass /
                                std::max(
                                    area * density,
                                    1.0e-12),
                            config.
                                maximumErosionDepthPerStepMeters);

                    if (desiredDepth > 0.0)
                    {
                        const auto removal =
                            result.material.Erode(
                                x,
                                y,
                                desiredDepth,
                                geology);

                        const auto* rock =
                            geology.Find(
                                result.material.
                                    At(x, y).
                                    bedrockMaterial);

                        if (rock == nullptr)
                        {
                            throw std::logic_error(
                                "Orbit M14 hydraulic classification lost M02 rock identity.");
                        }

                        const SedimentMass typed =
                            ClassifyRemovedMaterial(
                                removal,
                                result.material,
                                *rock,
                                SedimentSourceProcess::Hydraulic,
                                result.sedimentExchange->
                                    ConversionRules());

                        result.sedimentExchange->
                            PublishPhysicalRemoval(
                                x,
                                y,
                                SedimentTransportMedium::Waterborne,
                                typed);

                        shared =
                            result.sedimentExchange->
                                At(x, y).
                                waterborne;

                        state.suspendedSediment =
                            shared;

                        state.suspendedSedimentKg =
                            shared.TotalKg();

                        state.cumulativeErodedKg +=
                            removal.removedMassKg;

                        state.cumulativeErodedDepthMeters +=
                            removal.removedDepthMeters;
                    }
                }
                else if (
                    state.suspendedSedimentKg >
                        capacityKg &&
                    config.maximumDepositionDepthPerStepMeters >
                        0.0)
                {
                    const f64 excess =
                        state.
                            suspendedSedimentKg -
                        capacityKg;

                    const f64 exchangeFraction =
                        std::clamp(
                            config.
                                depositionRatePerSecond *
                                dt,
                            0.0,
                            1.0);

                    const f64 maximumMass =
                        config.
                            maximumDepositionDepthPerStepMeters *
                        area *
                        std::max({
                            static_cast<f64>(
                                result.material.Densities().
                                    debrisKgPerCubicMeter),
                            static_cast<f64>(
                                result.material.Densities().
                                    sandKgPerCubicMeter),
                            static_cast<f64>(
                                result.material.Densities().
                                    soilKgPerCubicMeter)
                        });

                    const f64 depositMass =
                        std::min(
                            excess *
                                exchangeFraction,
                            maximumMass);

                    if (depositMass > 0.0)
                    {
                        const auto deposited =
                            result.sedimentExchange->
                                DepositToColumn(
                                    result.material,
                                    x,
                                    y,
                                    SedimentTransportMedium::Waterborne,
                                    depositMass);

                        const f64 depositedMass =
                            deposited.DepositedKg();

                        const f64 depositDepth =
                            deposited.deposited.coarseDebrisKg /
                                std::max(
                                    area *
                                    static_cast<f64>(
                                        result.material.Densities().
                                            debrisKgPerCubicMeter),
                                    1.0e-12) +
                            deposited.deposited.sandKg /
                                std::max(
                                    area *
                                    static_cast<f64>(
                                        result.material.Densities().
                                            sandKgPerCubicMeter),
                                    1.0e-12) +
                            deposited.deposited.finesKg /
                                std::max(
                                    area *
                                    static_cast<f64>(
                                        result.material.Densities().
                                            soilKgPerCubicMeter),
                                    1.0e-12);

                        shared =
                            result.sedimentExchange->
                                At(x, y).
                                waterborne;

                        state.suspendedSediment =
                            shared;

                        state.suspendedSedimentKg =
                            shared.TotalKg();

                        state.cumulativeDepositedKg +=
                            depositedMass;

                        state.cumulativeDepositedDepthMeters +=
                            depositDepth;
                    }
                }

                sedimentAfterExchange[index] =
                    result.sedimentExchange->
                        At(x, y).
                        waterborne;
            }
        }

        // 5. Conservative sediment advection using the same virtual-pipe water
        // flux. Closed page boundaries mean no sediment disappears here.
        std::fill(
            sedimentNext.begin(),
            sedimentNext.end(),
            SedimentMass{});

        for (u32 y = 0; y < resolution; ++y)
        {
            for (u32 x = 0; x < resolution; ++x)
            {
                const std::size_t index =
                    Index(
                        resolution,
                        x,
                        y);

                const Flux4& flux =
                    nextFlux[index];

                const f64 totalFlux =
                    flux.Sum();

                const SedimentMass sourceSediment =
                    sedimentAfterExchange[index];

                if (sourceSediment.Empty() ||
                    totalFlux <= 0.0)
                {
                    sedimentNext[index] +=
                        sourceSediment;
                    continue;
                }

                const f64 sourceWaterVolume =
                    waterBeforeFlow[index] *
                    area;

                const f64 transportedFraction =
                    std::clamp(
                        dt *
                            totalFlux /
                            std::max(
                                sourceWaterVolume,
                                1.0e-12),
                        0.0,
                        1.0);

                const SedimentMass transportedMass =
                    ScaleSediment(
                        sourceSediment,
                        transportedFraction);

                sedimentNext[index] +=
                    sourceSediment -
                    transportedMass;

                const auto send =
                    [&](const Direction direction,
                        const i32 nx,
                        const i32 ny)
                    {
                        if (!Inside(nx, resolution) ||
                            !Inside(ny, resolution))
                        {
                            return;
                        }

                        const f64 directionFlux =
                            FluxToward(
                                flux,
                                direction);

                        if (directionFlux <= 0.0)
                        {
                            return;
                        }

                        const SedimentMass mass =
                            ScaleSediment(
                                transportedMass,
                                directionFlux /
                                    totalFlux);

                        sedimentNext[Index(
                            resolution,
                            static_cast<u32>(nx),
                            static_cast<u32>(ny))] +=
                                mass;

                        result.sedimentExchange->
                            RecordTransport(
                                x,
                                y,
                                nx,
                                ny,
                                SedimentTransportMedium::Waterborne,
                                mass);
                    };

                send(
                    Direction::West,
                    static_cast<i32>(x) - 1,
                    static_cast<i32>(y));

                send(
                    Direction::East,
                    static_cast<i32>(x) + 1,
                    static_cast<i32>(y));

                send(
                    Direction::North,
                    static_cast<i32>(x),
                    static_cast<i32>(y) - 1);

                send(
                    Direction::South,
                    static_cast<i32>(x),
                    static_cast<i32>(y) + 1);
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

                result.sedimentExchange->
                    At(x, y).
                    waterborne =
                        sedimentNext[index];

                result.cells[index].
                    suspendedSediment =
                        sedimentNext[index];

                result.cells[index].
                    suspendedSedimentKg =
                        sedimentNext[index].
                            TotalKg();
            }
        }

        // 6. Infiltration/moisture then evaporation.
        for (u32 y = 0; y < resolution; ++y)
        {
            for (u32 x = 0; x < resolution; ++x)
            {
                const std::size_t index =
                    Index(
                        resolution,
                        x,
                        y);

                HydraulicCellState& state =
                    result.cells[index];

                auto& column =
                    result.material.At(
                        x,
                        y);

                const f64 moistureDeficit =
                    std::max(
                        1.0 -
                            static_cast<f64>(
                                column.moisture),
                        0.0);

                const f64 permeability =
                    Permeability(
                        result.material,
                        geology,
                        x,
                        y);

                const f64 infiltration =
                    std::min(
                        state.waterDepthMeters,
                        config.
                            infiltrationMetersPerSecond *
                            dt *
                            (0.25 +
                             0.75 * permeability) *
                            moistureDeficit);

                state.waterDepthMeters =
                    std::max(
                        0.0,
                        state.waterDepthMeters -
                            infiltration);

                column.moisture =
                    static_cast<f32>(
                        std::clamp(
                            static_cast<f64>(
                                column.moisture) +
                                infiltration /
                                    config.
                                        moistureCapacityDepthMeters,
                            0.0,
                            1.0));

                const f64 evaporationFactor =
                    std::max(
                        0.0,
                        1.0 -
                            config.
                                evaporationRatePerSecond *
                            dt);

                state.waterDepthMeters *=
                    evaporationFactor;
            }
        }
    }

    f64 totalEroded = 0.0;
    f64 totalDeposited = 0.0;
    f64 finalSuspended = 0.0;

    for (const HydraulicCellState& state :
         result.cells)
    {
        totalEroded +=
            state.cumulativeErodedKg;

        totalDeposited +=
            state.cumulativeDepositedKg;

        finalSuspended +=
            state.suspendedSediment.
                TotalKg();
    }

    const f64 error =
        totalEroded -
        totalDeposited -
        finalSuspended;

    const f64 referenceMass =
        std::max(
            totalEroded,
            1.0);

    const auto finalMass =
        result.material.QueryMass(geology);

    const f64 physicalError =
        initialMass.LooseMassKg() +
        finalMass.excavatedBedrockKg -
        finalMass.LooseMassKg() -
        finalSuspended;

    const f64 physicalReference =
        std::max(
            initialMass.LooseMassKg() +
                finalMass.excavatedBedrockKg,
            1.0);

    result.massBalance = {
        .totalErodedKg = totalEroded,
        .totalDepositedKg = totalDeposited,
        .finalSuspendedKg = finalSuspended,
        .sedimentBoundaryLossKg = 0.0,
        .materialBalanceErrorKg = error,
        .materialBalanceRelativeError =
            std::abs(error) /
            referenceMass,
        .physicalColumnBalanceErrorKg =
            physicalError,
        .physicalColumnBalanceRelativeError =
            std::abs(physicalError) /
            physicalReference
    };

    return result;
}
} // namespace orbit::terrain_erosion
