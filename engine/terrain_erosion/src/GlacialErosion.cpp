#include <orbit/terrain_erosion/GlacialErosion.hpp>

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
struct FlowProposal
{
    std::size_t target{std::numeric_limits<std::size_t>::max()};
    i32 dx{0};
    i32 dy{0};

    f64 distanceMeters{1.0};
    f64 surfaceSlope{0.0};
    f64 speedMetersPerYear{0.0};
    f64 outgoingVolumeCubicMeters{0.0};
    f64 transportedFraction{0.0};
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

[[nodiscard]] SedimentMass ScaleSediment(
    const SedimentMass& mass,
    const f64 scale) noexcept
{
    return {
        .sandKg =
            mass.sandKg *
            scale,
        .finesKg =
            mass.finesKg *
            scale,
        .coarseDebrisKg =
            mass.coarseDebrisKg *
            scale
    };
}

[[nodiscard]] f64 IntrinsicGlacialErodibility(
    const terrain_material_column::MaterialColumnCell& cell,
    const terrain_geology::GeologicalMaterialLibrary& geology)
{
    if (cell.ExposedSurface() !=
        terrain_material_column::
            ExposedSurfaceKind::
                Bedrock)
    {
        return 1.0;
    }

    const auto* rock =
        geology.Find(
            cell.bedrockMaterial);

    if (rock == nullptr)
    {
        throw std::invalid_argument(
            "Orbit M15 encountered unknown M02 bedrock.");
    }

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

    const f64 fracture =
        std::clamp(
            static_cast<f64>(
                rock->fractureTendency),
            0.0,
            1.0);

    // Abrasion is reduced by competence while plucking grows with fracture.
    // Keep a small non-zero floor so competent rock still evolves geologically.
    return
        std::clamp(
            0.05 +
                0.45 *
                    (1.0 - hardness) +
                0.20 *
                    (1.0 - cohesion) +
                0.30 *
                    fracture,
            0.02,
            1.0);
}

[[nodiscard]] f64 ErosionDepthForCell(
    const terrain_material_column::MaterialColumnPage& material,
    const terrain_geology::GeologicalMaterialLibrary& geology,
    const std::span<const GlacialClimateCell> climate,
    const u32 x,
    const u32 y,
    const f64 iceThickness,
    const f64 speedMetersPerYear,
    const f64 processScale,
    const GlacialErosionConfig& config)
{
    if (iceThickness <
            config.
                minimumIceThicknessForErosionMeters ||
        speedMetersPerYear <= 0.0 ||
        processScale <= 0.0)
    {
        return 0.0;
    }

    const std::size_t index =
        Index(
            material.Resolution(),
            x,
            y);

    const f64 protectionAllowance =
        1.0 -
        std::clamp(
            static_cast<f64>(
                climate[index].
                    protection),
            0.0,
            1.0);

    if (protectionAllowance <= 0.0)
    {
        return 0.0;
    }

    const f64 speedFactor =
        std::pow(
            std::max(
                speedMetersPerYear /
                    config.
                        referenceErosionSpeedMetersPerYear,
                0.0),
            config.
                erosionSpeedExponent);

    const f64 thicknessFactor =
        std::pow(
            std::max(
                iceThickness /
                    config.
                        referenceErosionIceThicknessMeters,
                0.0),
            config.
                erosionThicknessExponent);

    const f64 geologyFactor =
        IntrinsicGlacialErodibility(
            material.At(
                x,
                y),
            geology);

    return
        std::min(
            config.
                maximumErosionDepthPerStepMeters,
            config.
                basalErosionMetersPerYearAtReference *
                config.
                    timeStepYears *
                speedFactor *
                thicknessFactor *
                geologyFactor *
                protectionAllowance *
                processScale);
}

[[nodiscard]] f64 SumIceVolume(
    const std::span<const f64> ice,
    const f64 area) noexcept
{
    f64 result = 0.0;

    for (const f64 thickness : ice)
    {
        result +=
            thickness *
            area;
    }

    return result;
}
} // namespace

bool GlacialClimateCell::IsValid() const noexcept
{
    return
        std::isfinite(
            meanAnnualTemperatureC) &&
        std::isfinite(
            snowfallMetersIceEquivalentPerYear) &&
        snowfallMetersIceEquivalentPerYear >=
            0.0F &&
        std::isfinite(
            initialIceThicknessMeters) &&
        initialIceThicknessMeters >=
            0.0F &&
        std::isfinite(
            processMask) &&
        processMask >= 0.0F &&
        processMask <= 1.0F &&
        std::isfinite(
            protection) &&
        protection >= 0.0F &&
        protection <= 1.0F;
}

bool GlacialErosionConfig::IsValid() const noexcept
{
    const auto positive =
        [](const f64 value)
        {
            return
                std::isfinite(value) &&
                value > 0.0;
        };

    const auto nonnegative =
        [](const f64 value)
        {
            return
                std::isfinite(value) &&
                value >= 0.0;
        };

    return
        iterations > 0U &&
        positive(timeStepYears) &&
        std::isfinite(
            maximumGlacierTemperatureC) &&
        positive(
            temperatureTransitionC) &&
        positive(
            snowfallForFullEligibilityMetersPerYear) &&
        std::isfinite(
            accumulationEfficiency) &&
        accumulationEfficiency >= 0.0 &&
        accumulationEfficiency <= 1.0 &&
        std::isfinite(
            meltStartTemperatureC) &&
        nonnegative(
            meltMetersIcePerYearPerDegreeC) &&
        nonnegative(
            maximumAblationMetersPerYear) &&
        nonnegative(
            minimumIceThicknessForFlowMeters) &&
        positive(
            referenceIceThicknessMeters) &&
        positive(
            referenceSurfaceSlope) &&
        nonnegative(
            referenceFlowSpeedMetersPerYear) &&
        nonnegative(
            iceThicknessFlowExponent) &&
        nonnegative(
            surfaceSlopeFlowExponent) &&
        nonnegative(
            maximumFlowSpeedMetersPerYear) &&
        std::isfinite(
            maximumFlowFractionPerStep) &&
        maximumFlowFractionPerStep >= 0.0 &&
        maximumFlowFractionPerStep <= 1.0 &&
        nonnegative(
            minimumIceThicknessForErosionMeters) &&
        nonnegative(
            basalErosionMetersPerYearAtReference) &&
        nonnegative(
            lateralErosionFraction) &&
        positive(
            referenceErosionSpeedMetersPerYear) &&
        positive(
            referenceErosionIceThicknessMeters) &&
        nonnegative(
            erosionSpeedExponent) &&
        nonnegative(
            erosionThicknessExponent) &&
        nonnegative(
            maximumErosionDepthPerStepMeters) &&
        nonnegative(
            debrisTransportEfficiency) &&
        nonnegative(
            moraineDepositionRatePerYear) &&
        positive(
            moraineThinIceThresholdMeters) &&
        positive(
            moraineStagnationSpeedMetersPerYear);
}

f32 EvaluateGlacialEligibility(
    const GlacialClimateCell& climate,
    const GlacialErosionConfig& config) noexcept
{
    if (!climate.IsValid() ||
        !config.IsValid() ||
        climate.processMask <= 0.0F)
    {
        return 0.0F;
    }

    const f64 cold =
        std::clamp(
            (config.
                 maximumGlacierTemperatureC -
             static_cast<f64>(
                 climate.
                     meanAnnualTemperatureC)) /
                config.
                    temperatureTransitionC,
            0.0,
            1.0);

    const f64 snow =
        std::clamp(
            static_cast<f64>(
                climate.
                    snowfallMetersIceEquivalentPerYear) /
                config.
                    snowfallForFullEligibilityMetersPerYear,
            0.0,
            1.0);

    return static_cast<f32>(
        std::clamp(
            cold *
                snow *
                static_cast<f64>(
                    climate.processMask),
            0.0,
            1.0));
}

const GlacialCellState&
GlacialErosionResult::At(
    const u32 x,
    const u32 y) const
{
    if (x >= material.Resolution() ||
        y >= material.Resolution())
    {
        throw std::out_of_range(
            "Orbit M15 glacial result coordinate is out of range.");
    }

    return
        cells[Index(
            material.Resolution(),
            x,
            y)];
}

GlacialErosionResult SimulateGlacialErosion(
    terrain_material_column::MaterialColumnPage material,
    const terrain_geology::GeologicalMaterialLibrary& geology,
    const std::span<const GlacialClimateCell> climate,
    const GlacialErosionConfig& config)
{
    if (!config.IsValid())
    {
        throw std::invalid_argument(
            "Orbit M15 glacial configuration is invalid.");
    }

    const u32 resolution =
        material.Resolution();

    const std::size_t cellCount =
        static_cast<std::size_t>(
            resolution) *
        resolution;

    if (climate.size() !=
        cellCount)
    {
        throw std::invalid_argument(
            "Orbit M15 climate field must match the physical page.");
    }

    for (const auto& cell :
         climate)
    {
        if (!cell.IsValid())
        {
            throw std::invalid_argument(
                "Orbit M15 climate field contains an invalid cell.");
        }
    }

    const auto initialMaterialMass =
        material.QueryMass(
            geology);

    GlacialErosionResult result{
        .material =
            std::move(material),
        .cells =
            std::vector<GlacialCellState>(
                cellCount)
    };

    result.sedimentExchange.emplace(
        resolution,
        result.material.
            SpacingMeters());

    std::vector<f64> eligibility(
        cellCount,
        0.0);

    std::vector<f64> ice(
        cellCount,
        0.0);

    std::vector<f64> iceAfterClimate(
        cellCount,
        0.0);

    std::vector<f64> nextIceVolume(
        cellCount,
        0.0);

    std::vector<FlowProposal> flow(
        cellCount);

    const f64 area =
        result.material.
            CellAreaSquareMeters();

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

            eligibility[index] =
                EvaluateGlacialEligibility(
                    climate[index],
                    config);

            ice[index] =
                eligibility[index] > 0.0
                    ? static_cast<f64>(
                          climate[index].
                              initialIceThicknessMeters) *
                          eligibility[index]
                    : 0.0;

            auto& state =
                result.cells[index];

            state.eligibility =
                static_cast<f32>(
                    eligibility[index]);

            state.iceThicknessMeters =
                static_cast<f32>(
                    ice[index]);

            state.iceSurfaceMeters =
                static_cast<f32>(
                    SurfaceHeight(
                        result.material,
                        x,
                        y) +
                    ice[index]);
        }
    }

    result.iceBalance.
        initialIceVolumeCubicMeters =
            SumIceVolume(
                ice,
                area);

    constexpr std::array<
        std::pair<i32, i32>,
        8>
        offsets{{
            {-1, -1},
            {0, -1},
            {1, -1},
            {-1, 0},
            {1, 0},
            {-1, 1},
            {0, 1},
            {1, 1}
        }};

    for (u32 iteration = 0U;
         iteration <
             config.iterations;
         ++iteration)
    {
        // 1. Climate accumulation/ablation. Eligibility zero is a hard process
        // boundary and therefore cannot retain or generate glacier ice.
        for (std::size_t index = 0U;
             index < cellCount;
             ++index)
        {
            if (eligibility[index] <= 0.0)
            {
                iceAfterClimate[index] =
                    0.0;
                continue;
            }

            const auto& forcing =
                climate[index];

            const f64 accumulationDepth =
                static_cast<f64>(
                    forcing.
                        snowfallMetersIceEquivalentPerYear) *
                config.
                    accumulationEfficiency *
                eligibility[index] *
                config.
                    timeStepYears;

            const f64 meltRate =
                std::min(
                    std::max(
                        static_cast<f64>(
                            forcing.
                                meanAnnualTemperatureC) -
                            config.
                                meltStartTemperatureC,
                        0.0) *
                        config.
                            meltMetersIcePerYearPerDegreeC,
                    config.
                        maximumAblationMetersPerYear);

            const f64 available =
                ice[index] +
                accumulationDepth;

            const f64 ablationDepth =
                std::min(
                    available,
                    meltRate *
                        eligibility[index] *
                        config.
                            timeStepYears);

            iceAfterClimate[index] =
                std::max(
                    available -
                        ablationDepth,
                    0.0);

            result.iceBalance.
                accumulatedIceVolumeCubicMeters +=
                    accumulationDepth *
                    area;

            result.iceBalance.
                ablatedIceVolumeCubicMeters +=
                    ablationDepth *
                    area;
        }

        // 2. Local steepest-descent shallow-ice approximation.
        std::fill(
            flow.begin(),
            flow.end(),
            FlowProposal{});

        std::fill(
            nextIceVolume.begin(),
            nextIceVolume.end(),
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

                const f64 thickness =
                    iceAfterClimate[index];

                nextIceVolume[index] +=
                    thickness *
                    area;

                if (eligibility[index] <= 0.0 ||
                    thickness <
                        config.
                            minimumIceThicknessForFlowMeters)
                {
                    continue;
                }

                const f64 sourceSurface =
                    SurfaceHeight(
                        result.material,
                        x,
                        y) +
                    thickness;

                f64 bestSlope = 0.0;
                f64 bestDistance = 1.0;
                i32 bestX = -1;
                i32 bestY = -1;
                i32 bestDx = 0;
                i32 bestDy = 0;

                for (const auto [dx, dy] :
                     offsets)
                {
                    const i32 nx =
                        static_cast<i32>(x) +
                        dx;

                    const i32 ny =
                        static_cast<i32>(y) +
                        dy;

                    if (!Inside(
                            nx,
                            ny,
                            resolution))
                    {
                        continue;
                    }

                    const std::size_t target =
                        Index(
                            resolution,
                            static_cast<u32>(nx),
                            static_cast<u32>(ny));

                    if (eligibility[target] <=
                        0.0)
                    {
                        continue;
                    }

                    const bool diagonal =
                        dx != 0 &&
                        dy != 0;

                    const f64 distance =
                        result.material.
                            SpacingMeters() *
                        (diagonal
                             ? 1.4142135623730951
                             : 1.0);

                    const f64 targetSurface =
                        SurfaceHeight(
                            result.material,
                            static_cast<u32>(nx),
                            static_cast<u32>(ny)) +
                        iceAfterClimate[target];

                    const f64 slope =
                        (sourceSurface -
                         targetSurface) /
                        distance;

                    if (slope >
                        bestSlope)
                    {
                        bestSlope =
                            slope;
                        bestDistance =
                            distance;
                        bestX = nx;
                        bestY = ny;
                        bestDx = dx;
                        bestDy = dy;
                    }
                }

                if (bestX < 0 ||
                    bestY < 0 ||
                    bestSlope <= 0.0)
                {
                    continue;
                }

                const f64 thicknessFactor =
                    std::pow(
                        std::max(
                            thickness /
                                config.
                                    referenceIceThicknessMeters,
                            0.0),
                        config.
                            iceThicknessFlowExponent);

                const f64 slopeFactor =
                    std::pow(
                        std::max(
                            bestSlope /
                                config.
                                    referenceSurfaceSlope,
                            0.0),
                        config.
                            surfaceSlopeFlowExponent);

                const f64 speed =
                    std::min(
                        config.
                            maximumFlowSpeedMetersPerYear,
                        config.
                            referenceFlowSpeedMetersPerYear *
                            thicknessFactor *
                            slopeFactor);

                const f64 transportedFraction =
                    std::clamp(
                        speed *
                            config.
                                timeStepYears /
                            bestDistance,
                        0.0,
                        config.
                            maximumFlowFractionPerStep);

                const f64 outgoingVolume =
                    thickness *
                    area *
                    transportedFraction;

                const std::size_t target =
                    Index(
                        resolution,
                        static_cast<u32>(bestX),
                        static_cast<u32>(bestY));

                nextIceVolume[index] -=
                    outgoingVolume;

                nextIceVolume[target] +=
                    outgoingVolume;

                flow[index] = {
                    .target =
                        target,
                    .dx =
                        bestDx,
                    .dy =
                        bestDy,
                    .distanceMeters =
                        bestDistance,
                    .surfaceSlope =
                        bestSlope,
                    .speedMetersPerYear =
                        speed,
                    .outgoingVolumeCubicMeters =
                        outgoingVolume,
                    .transportedFraction =
                        transportedFraction
                };
            }
        }

        for (std::size_t index = 0U;
             index < cellCount;
             ++index)
        {
            ice[index] =
                eligibility[index] > 0.0
                    ? std::max(
                          nextIceVolume[index] /
                              area,
                          0.0)
                    : 0.0;
        }

        // 3. Basal erosion and lateral widening. Lateral wall material is
        // entrained into the source glacier's M14 surface-mobile load.
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

                const auto& localFlow =
                    flow[index];

                state.flowDx =
                    localFlow.dx;

                state.flowDy =
                    localFlow.dy;

                state.flowSpeedMetersPerYear =
                    static_cast<f32>(
                        localFlow.
                            speedMetersPerYear);

                state.outgoingIceVolumeCubicMeters =
                    localFlow.
                        outgoingVolumeCubicMeters;

                const f64 basalDepth =
                    ErosionDepthForCell(
                        result.material,
                        geology,
                        climate,
                        x,
                        y,
                        ice[index],
                        localFlow.
                            speedMetersPerYear,
                        eligibility[index],
                        config);

                if (basalDepth > 0.0)
                {
                    const SedimentMass removed =
                        result.sedimentExchange->
                            PickupFromColumn(
                                result.material,
                                geology,
                                x,
                                y,
                                basalDepth,
                                SedimentSourceProcess::
                                    GlacialErosion,
                                SedimentTransportMedium::
                                    SurfaceMobile);

                    state.
                        cumulativeBasalErosionMeters +=
                            basalDepth;

                    state.
                        cumulativeErodedKg +=
                            removed.TotalKg();
                }

                if (basalDepth <= 0.0 ||
                    localFlow.dx == 0 &&
                        localFlow.dy == 0)
                {
                    continue;
                }

                const std::array<
                    std::pair<i32, i32>,
                    2>
                    sides{{
                        {-localFlow.dy,
                         localFlow.dx},
                        {localFlow.dy,
                         -localFlow.dx}
                    }};

                for (const auto [sx, sy] :
                     sides)
                {
                    const i32 nx =
                        static_cast<i32>(x) +
                        sx;

                    const i32 ny =
                        static_cast<i32>(y) +
                        sy;

                    if (!Inside(
                            nx,
                            ny,
                            resolution))
                    {
                        continue;
                    }

                    const std::size_t sideIndex =
                        Index(
                            resolution,
                            static_cast<u32>(nx),
                            static_cast<u32>(ny));

                    if (eligibility[sideIndex] <=
                        0.0)
                    {
                        continue;
                    }

                    const f64 sideDepth =
                        ErosionDepthForCell(
                            result.material,
                            geology,
                            climate,
                            static_cast<u32>(nx),
                            static_cast<u32>(ny),
                            ice[index],
                            localFlow.
                                speedMetersPerYear,
                            eligibility[index] *
                                config.
                                    lateralErosionFraction,
                            config);

                    if (sideDepth <= 0.0)
                    {
                        continue;
                    }

                    const SedimentMass removed =
                        result.sedimentExchange->
                            PickupFromColumn(
                                result.material,
                                geology,
                                static_cast<u32>(nx),
                                static_cast<u32>(ny),
                                sideDepth,
                                SedimentSourceProcess::
                                    GlacialErosion,
                                SedimentTransportMedium::
                                    SurfaceMobile);

                    const SedimentMass entrained =
                        result.sedimentExchange->
                            Take(
                                static_cast<u32>(nx),
                                static_cast<u32>(ny),
                                SedimentTransportMedium::
                                    SurfaceMobile,
                                removed);

                    result.sedimentExchange->
                        Add(
                            x,
                            y,
                            SedimentTransportMedium::
                                SurfaceMobile,
                            entrained);

                    state.
                        cumulativeLateralErosionMeters +=
                            sideDepth;

                    state.
                        cumulativeErodedKg +=
                            removed.TotalKg();
                }
            }
        }

        // 4. Advect the shared M14 surface-mobile debris with glacier flow.
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

                const FlowProposal& localFlow =
                    flow[index];

                if (localFlow.target ==
                        std::numeric_limits<
                            std::size_t>::max() ||
                    localFlow.
                        transportedFraction <=
                        0.0)
                {
                    continue;
                }

                const SedimentMass available =
                    result.sedimentExchange->
                        At(x, y).
                        surfaceMobile;

                if (available.Empty())
                {
                    continue;
                }

                const f64 fraction =
                    std::clamp(
                        localFlow.
                            transportedFraction *
                            config.
                                debrisTransportEfficiency,
                        0.0,
                        1.0);

                const SedimentMass requested =
                    ScaleSediment(
                        available,
                        fraction);

                const SedimentMass moved =
                    result.sedimentExchange->
                        Take(
                            x,
                            y,
                            SedimentTransportMedium::
                                SurfaceMobile,
                            requested);

                const u32 targetX =
                    static_cast<u32>(
                        localFlow.target %
                        resolution);

                const u32 targetY =
                    static_cast<u32>(
                        localFlow.target /
                        resolution);

                result.sedimentExchange->
                    Add(
                        targetX,
                        targetY,
                        SedimentTransportMedium::
                            SurfaceMobile,
                        moved);

                result.sedimentExchange->
                    RecordTransport(
                        x,
                        y,
                        static_cast<i32>(targetX),
                        static_cast<i32>(targetY),
                        SedimentTransportMedium::
                            SurfaceMobile,
                        moved);

                result.cells[index].
                    cumulativeTransportedSedimentKg +=
                        moved.TotalKg();
            }
        }

        // 5. Thin or stagnant ice deposits the shared debris load as moraine.
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

                if (eligibility[index] <=
                    0.0)
                {
                    continue;
                }

                const SedimentMass available =
                    result.sedimentExchange->
                        At(x, y).
                        surfaceMobile;

                if (available.Empty())
                {
                    continue;
                }

                const f64 thinFactor =
                    std::clamp(
                        1.0 -
                            ice[index] /
                                config.
                                    moraineThinIceThresholdMeters,
                        0.0,
                        1.0);

                const f64 stagnantFactor =
                    std::clamp(
                        1.0 -
                            flow[index].
                                speedMetersPerYear /
                                config.
                                    moraineStagnationSpeedMetersPerYear,
                        0.0,
                        1.0);

                const f64 settlingStrength =
                    std::max(
                        thinFactor,
                        stagnantFactor);

                const f64 depositFraction =
                    std::clamp(
                        config.
                            moraineDepositionRatePerYear *
                            config.
                                timeStepYears *
                            settlingStrength,
                        0.0,
                        1.0);

                if (depositFraction <= 0.0)
                {
                    continue;
                }

                const auto deposited =
                    result.sedimentExchange->
                        DepositToColumn(
                            result.material,
                            x,
                            y,
                            SedimentTransportMedium::
                                SurfaceMobile,
                            available.TotalKg() *
                                depositFraction);

                result.cells[index].
                    cumulativeMoraineDepositedKg +=
                        deposited.
                            DepositedKg();
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

                auto& state =
                    result.cells[index];

                state.iceThicknessMeters =
                    static_cast<f32>(
                        ice[index]);

                state.iceSurfaceMeters =
                    static_cast<f32>(
                        SurfaceHeight(
                            result.material,
                            x,
                            y) +
                        ice[index]);
            }
        }
    }

    result.iceBalance.
        finalIceVolumeCubicMeters =
            SumIceVolume(
                ice,
                area);

    result.iceBalance.
        balanceErrorCubicMeters =
            result.iceBalance.
                initialIceVolumeCubicMeters +
            result.iceBalance.
                accumulatedIceVolumeCubicMeters -
            result.iceBalance.
                ablatedIceVolumeCubicMeters -
            result.iceBalance.
                finalIceVolumeCubicMeters;

    const f64 iceReference =
        std::max(
            result.iceBalance.
                initialIceVolumeCubicMeters +
                result.iceBalance.
                    accumulatedIceVolumeCubicMeters,
            1.0);

    result.iceBalance.
        balanceRelativeError =
            std::abs(
                result.iceBalance.
                    balanceErrorCubicMeters) /
            iceReference;

    const auto finalMaterialMass =
        result.material.QueryMass(
            geology);

    const f64 newlyExcavatedBedrock =
        std::max(
            finalMaterialMass.
                excavatedBedrockKg -
                initialMaterialMass.
                    excavatedBedrockKg,
            0.0);

    const f64 finalMobile =
        result.sedimentExchange->
            TotalMobileMass().
            TotalKg();

    const f64 materialError =
        initialMaterialMass.
            LooseMassKg() +
        newlyExcavatedBedrock -
        finalMaterialMass.
            LooseMassKg() -
        finalMobile;

    const f64 materialReference =
        std::max(
            initialMaterialMass.
                LooseMassKg() +
                newlyExcavatedBedrock,
            1.0);

    result.materialBalance = {
        .initialLooseMassKg =
            initialMaterialMass.
                LooseMassKg(),
        .finalLooseMassKg =
            finalMaterialMass.
                LooseMassKg(),
        .newlyExcavatedBedrockKg =
            newlyExcavatedBedrock,
        .finalMobileSedimentKg =
            finalMobile,
        .depositedFromMobileKg =
            result.sedimentExchange->
                Accounting().
                mobileToPhysical.
                TotalKg(),
        .balanceErrorKg =
            materialError,
        .balanceRelativeError =
            std::abs(
                materialError) /
            materialReference
    };

    return result;
}
} // namespace orbit::terrain_erosion
