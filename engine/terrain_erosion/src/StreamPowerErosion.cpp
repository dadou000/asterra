#include <orbit/terrain_erosion/StreamPowerErosion.hpp>

#include <orbit/surface_authoring/TerrainConstraints.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace orbit::terrain_erosion
{
namespace
{
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

[[nodiscard]] f64 D8DistanceScale(
    const i8 dx,
    const i8 dy) noexcept
{
    return
        dx != 0 && dy != 0
            ? 1.4142135623730951
            : 1.0;
}

[[nodiscard]] const terrain_hydrology::DrainageBoundaryCell&
BoundaryTarget(
    const terrain_hydrology::DrainagePageHalo& halo,
    const u32 resolution,
    const i32 targetX,
    const i32 targetY)
{
    if (targetX < 0 && targetY < 0)
    {
        return halo.corners[0];
    }

    if (targetX >= static_cast<i32>(resolution) &&
        targetY < 0)
    {
        return halo.corners[1];
    }

    if (targetX >= static_cast<i32>(resolution) &&
        targetY >= static_cast<i32>(resolution))
    {
        return halo.corners[2];
    }

    if (targetX < 0 &&
        targetY >= static_cast<i32>(resolution))
    {
        return halo.corners[3];
    }

    if (targetY < 0)
    {
        return halo.north[
            static_cast<std::size_t>(
                std::clamp(
                    targetX,
                    0,
                    static_cast<i32>(resolution) - 1))];
    }

    if (targetX >= static_cast<i32>(resolution))
    {
        return halo.east[
            static_cast<std::size_t>(
                std::clamp(
                    targetY,
                    0,
                    static_cast<i32>(resolution) - 1))];
    }

    if (targetY >= static_cast<i32>(resolution))
    {
        return halo.south[
            static_cast<std::size_t>(
                std::clamp(
                    targetX,
                    0,
                    static_cast<i32>(resolution) - 1))];
    }

    if (targetX < 0)
    {
        return halo.west[
            static_cast<std::size_t>(
                std::clamp(
                    targetY,
                    0,
                    static_cast<i32>(resolution) - 1))];
    }

    throw std::logic_error(
        "Orbit M10 requested an interior cell through the boundary helper.");
}

[[nodiscard]] f64 BedrockErodibility(
    const terrain_geology::GeologicalMaterial& rock) noexcept
{
    const auto response =
        terrain_geology::EvaluateIntrinsicErosionResponse(
            rock,
            {
                .hydraulic = 1.0F,
                .aeolian = 0.0F,
                .chemical = 0.0F,
                .fracture = 0.0F
            });

    // Hydraulic erodibility remains the primary M02 coefficient. Hardness and
    // cohesion further suppress incision without redefining geological data.
    const f64 competence =
        (1.0 -
         0.50 *
             std::clamp(
                 static_cast<f64>(
                     rock.hardness),
                 0.0,
                 1.0)) *
        (1.0 -
         0.25 *
             std::clamp(
                 static_cast<f64>(
                     rock.cohesion),
                 0.0,
                 1.0));

    return
        std::clamp(
            static_cast<f64>(
                response.hydraulicDetachment) *
                competence,
            0.0,
            1.0);
}

[[nodiscard]] f64 CellErodibility(
    const terrain_material_column::MaterialColumnCell& cell,
    const terrain_geology::GeologicalMaterialLibrary& geology,
    const StreamPowerErosionConfig& config)
{
    if (cell.ExposedSurface() !=
        terrain_material_column::ExposedSurfaceKind::Bedrock)
    {
        return
            std::clamp(
                config.looseMaterialErodibility,
                0.0,
                1.0);
    }

    const terrain_geology::GeologicalMaterial* rock =
        geology.Find(
            cell.bedrockMaterial);

    if (rock == nullptr)
    {
        throw std::invalid_argument(
            "Orbit M10 encountered an M08 bedrock material missing from "
            "the M02 geological library.");
    }

    return BedrockErodibility(*rock);
}

[[nodiscard]] f64 DrainageMeasure(
    const terrain_hydrology::DrainageCell& cell,
    const StreamPowerErosionConfig& config) noexcept
{
    if (config.drainageMeasure ==
        StreamPowerDrainageMeasure::Discharge)
    {
        return
            cell.dischargeCubicMetersPerSecond /
            config.referenceDischargeCubicMetersPerSecond;
    }

    return
        cell.drainageAreaSquareMeters /
        config.referenceDrainageAreaSquareMeters;
}

[[nodiscard]] f64 ConstraintTarget(
    const f64 initialSurface,
    const StreamPowerCellForcing& forcing) noexcept
{
    return
        initialSurface +
        forcing.authoredElevationOffsetMeters;
}

[[nodiscard]] f64 SampleCoordinate(
    const f64 minimum,
    const f64 maximum,
    const u32 coordinate,
    const u32 resolution) noexcept
{
    if (resolution <= 1U)
    {
        return
            (minimum + maximum) *
            0.5;
    }

    const f64 t =
        static_cast<f64>(coordinate) /
        static_cast<f64>(resolution - 1U);

    return
        minimum +
        (maximum - minimum) *
            t;
}

[[nodiscard]] u64 CombineDouble(
    const u64 seed,
    const f64 value) noexcept
{
    return
        terrain::StableCombine64(
            seed,
            std::bit_cast<u64>(value));
}
} // namespace

bool StreamPowerErosionConfig::IsValid() const noexcept
{
    return
        iterations > 0U &&
        std::isfinite(
            upliftCouplingPerIteration) &&
        upliftCouplingPerIteration >= 0.0 &&
        std::isfinite(
            authoredHeightRelaxation) &&
        authoredHeightRelaxation >= 0.0 &&
        authoredHeightRelaxation <= 1.0 &&
        std::isfinite(
            incisionCoefficientMetersPerIteration) &&
        incisionCoefficientMetersPerIteration >= 0.0 &&
        std::isfinite(
            drainageExponent) &&
        drainageExponent >= 0.0 &&
        std::isfinite(
            slopeExponent) &&
        slopeExponent >= 0.0 &&
        std::isfinite(
            referenceDrainageAreaSquareMeters) &&
        referenceDrainageAreaSquareMeters > 0.0 &&
        std::isfinite(
            referenceDischargeCubicMetersPerSecond) &&
        referenceDischargeCubicMetersPerSecond > 0.0 &&
        std::isfinite(
            looseMaterialErodibility) &&
        looseMaterialErodibility >= 0.0 &&
        looseMaterialErodibility <= 1.0 &&
        std::isfinite(
            minimumBedSlope) &&
        minimumBedSlope >= 0.0 &&
        std::isfinite(
            maximumIncisionMetersPerIteration) &&
        maximumIncisionMetersPerIteration >= 0.0 &&
        drainage.IsValid();
}

bool StreamPowerCellForcing::IsValid() const noexcept
{
    return
        std::isfinite(
            upliftForcingMeters) &&
        std::isfinite(
            authoredElevationOffsetMeters) &&
        std::isfinite(
            protection) &&
        protection >= 0.0 &&
        protection <= 1.0;
}

const StreamPowerCellResult&
StreamPowerErosionResult::At(
    const u32 x,
    const u32 y) const
{
    if (x >= resolution ||
        y >= resolution)
    {
        throw std::out_of_range(
            "Orbit M10 stream-power result coordinate is out of range.");
    }

    return
        cells[Index(
            resolution,
            x,
            y)];
}

std::vector<StreamPowerCellForcing>
BuildStreamPowerForcing(
    const terrain_material_column::MaterialColumnPage& materialColumn,
    const terrain::PhysicalTerrainPageKey& sourcePage,
    const world::PlanetDefinition& planet,
    const terrain_macro_geology::MacroGeologyField& macroGeology)
{
    const u32 resolution =
        materialColumn.Resolution();

    if (resolution == 0U ||
        sourcePage.resolution != resolution ||
        !planet.id.IsValid() ||
        sourcePage.address.planet != planet.id)
    {
        throw std::invalid_argument(
            "Orbit M10 forcing builder requires a matching M08 page and "
            "planet identity.");
    }

    const world::CubeBounds bounds =
        world::TileBounds(
            sourcePage.address.tile);

    std::vector<StreamPowerCellForcing> forcing(
        static_cast<std::size_t>(resolution) *
        resolution);

    for (u32 y = 0; y < resolution; ++y)
    {
        const f64 v =
            SampleCoordinate(
                bounds.minimumUv.y,
                bounds.maximumUv.y,
                y,
                resolution);

        for (u32 x = 0; x < resolution; ++x)
        {
            const f64 u =
                SampleCoordinate(
                    bounds.minimumUv.x,
                    bounds.maximumUv.x,
                    x,
                    resolution);

            const math::Double3 direction =
                world::CubeToUnitDirection({
                    .face = bounds.face,
                    .uv = {u, v}
                });

            const terrain::PlanetSurfacePosition position =
                terrain::CanonicalizeSurfacePosition({
                    .planet = planet.id,
                    .unitDirection = direction
                });

            const terrain_macro_geology::MacroGeologySample sample =
                macroGeology.Sample(position);

            forcing[Index(
                resolution,
                x,
                y)] = {
                .upliftForcingMeters =
                    sample.upliftMeters,
                .authoredElevationOffsetMeters =
                    sample.authoredHeightMeters,
                .protection =
                    std::clamp(
                        sample.protection,
                        0.0,
                        1.0)
            };
        }
    }

    return forcing;
}

u64 StreamPowerRevisionFingerprint(
    const terrain::PhysicalTerrainPageKey& sourcePage,
    const u64 geologyRevision,
    const u64 drainageHaloRevision,
    const std::span<const StreamPowerCellForcing> forcing,
    const StreamPowerErosionConfig& config) noexcept
{
    u64 value =
        terrain::StableCombine64(
            0x4F52424954535045ULL, // "ORBITSPE"
            terrain::PhysicalPageFingerprint(
                sourcePage));

    value =
        terrain::StableCombine64(
            value,
            geologyRevision);

    value =
        terrain::StableCombine64(
            value,
            drainageHaloRevision);

    value =
        terrain::StableCombine64(
            value,
            config.iterations);

    value =
        CombineDouble(
            value,
            config.upliftCouplingPerIteration);

    value =
        CombineDouble(
            value,
            config.authoredHeightRelaxation);

    value =
        CombineDouble(
            value,
            config.incisionCoefficientMetersPerIteration);

    value =
        CombineDouble(
            value,
            config.drainageExponent);

    value =
        CombineDouble(
            value,
            config.slopeExponent);

    value =
        terrain::StableCombine64(
            value,
            static_cast<u64>(
                config.drainageMeasure));

    value =
        CombineDouble(
            value,
            config.referenceDrainageAreaSquareMeters);

    value =
        CombineDouble(
            value,
            config.referenceDischargeCubicMetersPerSecond);

    value =
        CombineDouble(
            value,
            config.looseMaterialErodibility);

    value =
        CombineDouble(
            value,
            config.minimumBedSlope);

    value =
        CombineDouble(
            value,
            config.maximumIncisionMetersPerIteration);

    value =
        terrain::StableCombine64(
            value,
            static_cast<u64>(
                config.drainage.depressionPolicy));

    value =
        terrain::StableCombine64(
            value,
            std::bit_cast<u32>(
                config.drainage.minimumDrainageDropMeters));

    value =
        terrain::StableCombine64(
            value,
            std::bit_cast<u32>(
                config.drainage.authoredGuidanceWeight));

    for (const StreamPowerCellForcing& cell :
         forcing)
    {
        value =
            CombineDouble(
                value,
                cell.upliftForcingMeters);

        value =
            CombineDouble(
                value,
                cell.authoredElevationOffsetMeters);

        value =
            CombineDouble(
                value,
                cell.protection);
    }

    return value;
}

StreamPowerErosionResult SolveStreamPowerErosion(
    const terrain_material_column::MaterialColumnPage& materialColumn,
    const terrain::PhysicalTerrainPageKey& sourcePage,
    const terrain_geology::GeologicalMaterialLibrary& geology,
    const std::span<const terrain_hydrology::DrainageCellInput>
        drainageInputs,
    const terrain_hydrology::DrainagePageHalo& drainageHalo,
    const std::span<const StreamPowerCellForcing> forcing,
    const StreamPowerErosionConfig& config)
{
    if (!config.IsValid())
    {
        throw std::invalid_argument(
            "Orbit M10 stream-power configuration is invalid.");
    }

    const u32 resolution =
        materialColumn.Resolution();

    const std::size_t cellCount =
        static_cast<std::size_t>(resolution) *
        resolution;

    if (resolution == 0U ||
        sourcePage.resolution != resolution ||
        drainageInputs.size() != cellCount ||
        forcing.size() != cellCount ||
        !drainageHalo.IsComplete(resolution))
    {
        throw std::invalid_argument(
            "Orbit M10 requires matching M08/M09 page inputs and a complete "
            "seam boundary halo.");
    }

    for (const StreamPowerCellForcing& cell :
         forcing)
    {
        if (!cell.IsValid())
        {
            throw std::invalid_argument(
                "Orbit M10 forcing contains an invalid cell.");
        }
    }

    terrain_material_column::MaterialColumnPage working =
        materialColumn;

    StreamPowerErosionResult result{
        .sourcePage = sourcePage,
        .revision =
            StreamPowerRevisionFingerprint(
                sourcePage,
                geology.Revision(),
                drainageHalo.revision,
                forcing,
                config),
        .resolution = resolution,
        .spacingMeters =
            materialColumn.SpacingMeters(),
        .cells =
            std::vector<StreamPowerCellResult>(
                cellCount)
    };

    std::vector<f64> initialSurface(
        cellCount,
        0.0);

    std::vector<f64> displacementStep(
        cellCount,
        0.0);

    std::vector<f64> incisionStep(
        cellCount,
        0.0);

    std::vector<f64> lastStreamPower(
        cellCount,
        0.0);

    std::vector<f64> lastSlope(
        cellCount,
        0.0);

    std::vector<f64> lastErodibility(
        cellCount,
        0.0);

    for (u32 y = 0; y < resolution; ++y)
    {
        for (u32 x = 0; x < resolution; ++x)
        {
            const std::size_t index =
                Index(
                    resolution,
                    x,
                    y);

            initialSurface[index] =
                static_cast<f64>(
                    materialColumn.
                        At(x, y).
                        SurfaceHeightMeters());

            result.cells[index].
                initialSurfaceHeightMeters =
                    static_cast<f32>(
                        initialSurface[index]);
        }
    }

    for (u32 iteration = 0;
         iteration < config.iterations;
         ++iteration)
    {
        const terrain_hydrology::DrainagePage drainage =
            terrain_hydrology::BuildDrainagePage(
                working,
                sourcePage,
                drainageInputs,
                drainageHalo,
                config.drainage);

        // First compute geological/authored displacement for every cell.
        for (u32 y = 0; y < resolution; ++y)
        {
            for (u32 x = 0; x < resolution; ++x)
            {
                const std::size_t index =
                    Index(
                        resolution,
                        x,
                        y);

                const f64 currentSurface =
                    static_cast<f64>(
                        working.
                            At(x, y).
                            SurfaceHeightMeters());

                const f64 tectonic =
                    forcing[index].
                        upliftForcingMeters *
                    config.
                        upliftCouplingPerIteration;

                const f64 target =
                    ConstraintTarget(
                        initialSurface[index],
                        forcing[index]);

                const f64 authoredCorrection =
                    (target -
                     currentSurface) *
                    config.
                        authoredHeightRelaxation;

                displacementStep[index] =
                    tectonic +
                    authoredCorrection;
            }
        }

        std::fill(
            incisionStep.begin(),
            incisionStep.end(),
            0.0);

        for (u32 y = 0; y < resolution; ++y)
        {
            for (u32 x = 0; x < resolution; ++x)
            {
                const std::size_t index =
                    Index(
                        resolution,
                        x,
                        y);

                const terrain_hydrology::DrainageCell& hydro =
                    drainage.At(x, y);

                lastStreamPower[index] = 0.0;
                lastSlope[index] = 0.0;

                const f64 erodibility =
                    CellErodibility(
                        working.At(x, y),
                        geology,
                        config);

                lastErodibility[index] =
                    erodibility;

                if (!hydro.flow.HasDownstream() ||
                    erodibility <= 0.0)
                {
                    continue;
                }

                const i32 targetX =
                    static_cast<i32>(x) +
                    hydro.flow.dx;

                const i32 targetY =
                    static_cast<i32>(y) +
                    hydro.flow.dy;

                const f64 distance =
                    working.SpacingMeters() *
                    D8DistanceScale(
                        hydro.flow.dx,
                        hydro.flow.dy);

                if (!(distance > 0.0))
                {
                    continue;
                }

                f64 downstreamDrainageElevation = 0.0;
                f64 downstreamPredictedSurface = 0.0;

                if (targetX >= 0 &&
                    targetY >= 0 &&
                    targetX <
                        static_cast<i32>(
                            resolution) &&
                    targetY <
                        static_cast<i32>(
                            resolution))
                {
                    const u32 nx =
                        static_cast<u32>(
                            targetX);

                    const u32 ny =
                        static_cast<u32>(
                            targetY);

                    downstreamDrainageElevation =
                        static_cast<f64>(
                            drainage.
                                At(nx, ny).
                                drainageElevationMeters);

                    downstreamPredictedSurface =
                        static_cast<f64>(
                            working.
                                At(nx, ny).
                                SurfaceHeightMeters()) +
                        displacementStep[
                            Index(
                                resolution,
                                nx,
                                ny)];
                }
                else
                {
                    const auto& boundary =
                        BoundaryTarget(
                            drainageHalo,
                            resolution,
                            targetX,
                            targetY);

                    downstreamDrainageElevation =
                        static_cast<f64>(
                            boundary.
                                conditionedHeightMeters);

                    downstreamPredictedSurface =
                        static_cast<f64>(
                            boundary.
                                surfaceHeightMeters);
                }

                const f64 slope =
                    std::max(
                        (static_cast<f64>(
                             hydro.
                                 drainageElevationMeters) -
                         downstreamDrainageElevation) /
                            distance,
                        0.0);

                lastSlope[index] =
                    slope;

                const f64 normalizedMeasure =
                    std::max(
                        DrainageMeasure(
                            hydro,
                            config),
                        0.0);

                const f64 protectedErodibility =
                    erodibility *
                    surface_authoring::
                        ErosionAllowanceFromProtection(
                            forcing[index].
                                protection);

                const f64 effectiveSlope =
                    std::max(
                        slope,
                        config.
                            minimumBedSlope);

                const f64 streamPower =
                    config.
                        incisionCoefficientMetersPerIteration *
                    std::pow(
                        normalizedMeasure,
                        config.
                            drainageExponent) *
                    std::pow(
                        effectiveSlope,
                        config.
                            slopeExponent) *
                    protectedErodibility;

                lastStreamPower[index] =
                    streamPower;

                const f64 sourcePredictedSurface =
                    static_cast<f64>(
                        working.
                            At(x, y).
                            SurfaceHeightMeters()) +
                    displacementStep[index];

                const f64 minimumSourceSurface =
                    downstreamPredictedSurface +
                    config.minimumBedSlope *
                        distance;

                const f64 incisionCapacity =
                    std::max(
                        sourcePredictedSurface -
                            minimumSourceSurface,
                        0.0);

                incisionStep[index] =
                    std::min({
                        std::max(
                            streamPower,
                            0.0),
                        config.
                            maximumIncisionMetersPerIteration,
                        incisionCapacity
                    });
            }
        }

        // Apply geological movement first so M08's excavation reference moves
        // with tectonic/authored displacement. Incision then removes the
        // canonical debris->sand->soil->regolith->bedrock stack.
        for (u32 y = 0; y < resolution; ++y)
        {
            for (u32 x = 0; x < resolution; ++x)
            {
                const std::size_t index =
                    Index(
                        resolution,
                        x,
                        y);

                const f64 displacement =
                    displacementStep[index];

                if (displacement != 0.0)
                {
                    working.DisplaceBedrock(
                        x,
                        y,
                        displacement);

                    result.cells[index].
                        cumulativeBedrockDisplacementMeters +=
                            static_cast<f32>(
                                displacement);
                }
            }
        }

        for (u32 y = 0; y < resolution; ++y)
        {
            for (u32 x = 0; x < resolution; ++x)
            {
                const std::size_t index =
                    Index(
                        resolution,
                        x,
                        y);

                const f64 incision =
                    incisionStep[index];

                if (incision <= 0.0)
                {
                    continue;
                }

                const auto removal =
                    working.Erode(
                        x,
                        y,
                        incision,
                        geology);

                result.cells[index].
                    cumulativeIncisionMeters +=
                        static_cast<f32>(
                            removal.
                                removedDepthMeters);
            }
        }
    }

    const terrain_hydrology::DrainagePage finalDrainage =
        terrain_hydrology::BuildDrainagePage(
            working,
            sourcePage,
            drainageInputs,
            drainageHalo,
            config.drainage);

    for (u32 y = 0; y < resolution; ++y)
    {
        for (u32 x = 0; x < resolution; ++x)
        {
            const std::size_t index =
                Index(
                    resolution,
                    x,
                    y);

            const terrain_hydrology::DrainageCell& hydro =
                finalDrainage.At(
                    x,
                    y);

            StreamPowerCellResult& output =
                result.cells[index];

            output.finalSurfaceHeightMeters =
                working.
                    At(x, y).
                    SurfaceHeightMeters();

            output.lastStreamPowerMetersPerIteration =
                static_cast<f32>(
                    lastStreamPower[index]);

            output.lastSlope =
                static_cast<f32>(
                    lastSlope[index]);

            output.lastErodibility =
                static_cast<f32>(
                    lastErodibility[index]);

            output.finalDrainageAreaSquareMeters =
                hydro.
                    drainageAreaSquareMeters;

            output.finalDischargeCubicMetersPerSecond =
                hydro.
                    dischargeCubicMetersPerSecond;
        }
    }

    return result;
}

StreamPowerBakeSummary ApplyStreamPowerErosionResult(
    terrain_material_column::MaterialColumnPage& materialColumn,
    const terrain_geology::GeologicalMaterialLibrary& geology,
    const StreamPowerErosionResult& result)
{
    if (materialColumn.Resolution() !=
            result.resolution ||
        result.cells.size() !=
            static_cast<std::size_t>(
                result.resolution) *
            result.resolution ||
        materialColumn.SpacingMeters() !=
            result.spacingMeters)
    {
        throw std::invalid_argument(
            "Orbit M10 bake target does not match the solved physical page.");
    }

    StreamPowerBakeSummary summary{};

    // Displacement is geological movement, not erosion.
    for (u32 y = 0; y < result.resolution; ++y)
    {
        for (u32 x = 0; x < result.resolution; ++x)
        {
            const StreamPowerCellResult& cell =
                result.At(
                    x,
                    y);

            const f64 displacement =
                static_cast<f64>(
                    cell.
                        cumulativeBedrockDisplacementMeters);

            if (displacement != 0.0)
            {
                materialColumn.DisplaceBedrock(
                    x,
                    y,
                    displacement);
            }

            summary.totalBedrockDisplacementMeters +=
                displacement;
        }
    }

    // Incision always goes through M08's top-down material removal contract.
    for (u32 y = 0; y < result.resolution; ++y)
    {
        for (u32 x = 0; x < result.resolution; ++x)
        {
            const StreamPowerCellResult& cell =
                result.At(
                    x,
                    y);

            const f64 incision =
                std::max(
                    static_cast<f64>(
                        cell.
                            cumulativeIncisionMeters),
                    0.0);

            if (incision <= 0.0)
            {
                continue;
            }

            const auto removal =
                materialColumn.Erode(
                    x,
                    y,
                    incision,
                    geology);

            summary.totalIncisionMeters +=
                removal.removedDepthMeters;

            summary.removedMassKg +=
                removal.removedMassKg;
        }
    }

    return summary;
}
} // namespace orbit::terrain_erosion
