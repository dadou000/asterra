#include <orbit/terrain_water/CoastalProcess.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace orbit::terrain_water
{
namespace
{
constexpr f64 kPi = 3.1415926535897932384626433832795;

enum class Side : u8
{
    North,
    East,
    South,
    West
};

struct Conserved
{
    f64 h{0.0};
    f64 qx{0.0};
    f64 qy{0.0};
};

struct InterfaceFlux
{
    Conserved left{};
    Conserved right{};
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

[[nodiscard]] f64 Length(
    const math::Double2& v) noexcept
{
    return
        std::hypot(
            v.x,
            v.y);
}

[[nodiscard]] math::Double2 Normalize(
    const math::Double2& v) noexcept
{
    const f64 length =
        Length(v);

    if (length <= 1.0e-12)
    {
        return {};
    }

    return {
        v.x / length,
        v.y / length
    };
}

[[nodiscard]] f64 SurfaceHeight(
    const terrain_material_column::MaterialColumnPage& material,
    const u32 x,
    const u32 y) noexcept
{
    return
        static_cast<f64>(
            material.
                At(x, y).
                SurfaceHeightMeters());
}

[[nodiscard]] f64 TotalWaterVolume(
    const CoastalWaterPage& page) noexcept
{
    const f64 area =
        page.spacingMeters *
        page.spacingMeters;

    f64 total = 0.0;

    for (const auto& cell :
         page.cells)
    {
        total +=
            std::max(
                cell.waterDepthMeters,
                0.0) *
            area;
    }

    return total;
}

[[nodiscard]] f64 VelocityComponent(
    const f64 momentum,
    const f64 depth,
    const f64 dryThreshold) noexcept
{
    if (depth <=
        dryThreshold)
    {
        return 0.0;
    }

    return
        momentum /
        depth;
}

[[nodiscard]] Conserved PhysicalFluxX(
    const Conserved& state,
    const f64 gravity,
    const f64 dryThreshold) noexcept
{
    if (state.h <=
        dryThreshold)
    {
        return {};
    }

    const f64 u =
        state.qx /
        state.h;

    const f64 v =
        state.qy /
        state.h;

    return {
        .h =
            state.qx,
        .qx =
            state.qx * u +
            0.5 *
                gravity *
                state.h *
                state.h,
        .qy =
            state.qx * v
    };
}

[[nodiscard]] Conserved PhysicalFluxY(
    const Conserved& state,
    const f64 gravity,
    const f64 dryThreshold) noexcept
{
    if (state.h <=
        dryThreshold)
    {
        return {};
    }

    const f64 u =
        state.qx /
        state.h;

    const f64 v =
        state.qy /
        state.h;

    return {
        .h =
            state.qy,
        .qx =
            state.qy * u,
        .qy =
            state.qy * v +
            0.5 *
                gravity *
                state.h *
                state.h
    };
}

[[nodiscard]] Conserved ScaleStateForDepth(
    const Conserved& state,
    const f64 depth,
    const f64 dryThreshold) noexcept
{
    if (depth <=
            dryThreshold ||
        state.h <=
            dryThreshold)
    {
        return {
            .h = depth
        };
    }

    const f64 ratio =
        depth /
        state.h;

    return {
        .h = depth,
        .qx =
            state.qx *
            ratio,
        .qy =
            state.qy *
            ratio
    };
}

[[nodiscard]] InterfaceFlux FluxX(
    const Conserved& left,
    const f64 leftBed,
    const Conserved& right,
    const f64 rightBed,
    const CoastalShallowWaterConfig& config) noexcept
{
    const f64 leftSurface =
        leftBed +
        left.h;

    const f64 rightSurface =
        rightBed +
        right.h;

    const f64 interfaceBed =
        std::max(
            leftBed,
            rightBed);

    const f64 leftDepth =
        std::max(
            leftSurface -
                interfaceBed,
            0.0);

    const f64 rightDepth =
        std::max(
            rightSurface -
                interfaceBed,
            0.0);

    const Conserved leftStar =
        ScaleStateForDepth(
            left,
            leftDepth,
            config.
                dryThresholdMeters);

    const Conserved rightStar =
        ScaleStateForDepth(
            right,
            rightDepth,
            config.
                dryThresholdMeters);

    const Conserved fLeft =
        PhysicalFluxX(
            leftStar,
            config.
                gravityMetersPerSecondSquared,
            config.
                dryThresholdMeters);

    const Conserved fRight =
        PhysicalFluxX(
            rightStar,
            config.
                gravityMetersPerSecondSquared,
            config.
                dryThresholdMeters);

    const f64 uLeft =
        VelocityComponent(
            leftStar.qx,
            leftStar.h,
            config.
                dryThresholdMeters);

    const f64 uRight =
        VelocityComponent(
            rightStar.qx,
            rightStar.h,
            config.
                dryThresholdMeters);

    const f64 waveLeft =
        std::sqrt(
            config.
                gravityMetersPerSecondSquared *
            std::max(
                leftStar.h,
                0.0));

    const f64 waveRight =
        std::sqrt(
            config.
                gravityMetersPerSecondSquared *
            std::max(
                rightStar.h,
                0.0));

    const f64 signal =
        std::max(
            std::abs(uLeft) +
                waveLeft,
            std::abs(uRight) +
                waveRight);

    const Conserved numerical{
        .h =
            0.5 *
                (fLeft.h +
                 fRight.h) -
            0.5 *
                signal *
                (rightStar.h -
                 leftStar.h),
        .qx =
            0.5 *
                (fLeft.qx +
                 fRight.qx) -
            0.5 *
                signal *
                (rightStar.qx -
                 leftStar.qx),
        .qy =
            0.5 *
                (fLeft.qy +
                 fRight.qy) -
            0.5 *
                signal *
                (rightStar.qy -
                 leftStar.qy)
    };

    const f64 leftCorrection =
        0.5 *
        config.
            gravityMetersPerSecondSquared *
        (left.h *
             left.h -
         leftStar.h *
             leftStar.h);

    const f64 rightCorrection =
        0.5 *
        config.
            gravityMetersPerSecondSquared *
        (right.h *
             right.h -
         rightStar.h *
             rightStar.h);

    return {
        .left = {
            .h =
                numerical.h,
            .qx =
                numerical.qx +
                leftCorrection,
            .qy =
                numerical.qy
        },
        .right = {
            .h =
                numerical.h,
            .qx =
                numerical.qx +
                rightCorrection,
            .qy =
                numerical.qy
        }
    };
}

[[nodiscard]] InterfaceFlux FluxY(
    const Conserved& north,
    const f64 northBed,
    const Conserved& south,
    const f64 southBed,
    const CoastalShallowWaterConfig& config) noexcept
{
    const f64 northSurface =
        northBed +
        north.h;

    const f64 southSurface =
        southBed +
        south.h;

    const f64 interfaceBed =
        std::max(
            northBed,
            southBed);

    const f64 northDepth =
        std::max(
            northSurface -
                interfaceBed,
            0.0);

    const f64 southDepth =
        std::max(
            southSurface -
                interfaceBed,
            0.0);

    const Conserved northStar =
        ScaleStateForDepth(
            north,
            northDepth,
            config.
                dryThresholdMeters);

    const Conserved southStar =
        ScaleStateForDepth(
            south,
            southDepth,
            config.
                dryThresholdMeters);

    const Conserved fNorth =
        PhysicalFluxY(
            northStar,
            config.
                gravityMetersPerSecondSquared,
            config.
                dryThresholdMeters);

    const Conserved fSouth =
        PhysicalFluxY(
            southStar,
            config.
                gravityMetersPerSecondSquared,
            config.
                dryThresholdMeters);

    const f64 vNorth =
        VelocityComponent(
            northStar.qy,
            northStar.h,
            config.
                dryThresholdMeters);

    const f64 vSouth =
        VelocityComponent(
            southStar.qy,
            southStar.h,
            config.
                dryThresholdMeters);

    const f64 waveNorth =
        std::sqrt(
            config.
                gravityMetersPerSecondSquared *
            std::max(
                northStar.h,
                0.0));

    const f64 waveSouth =
        std::sqrt(
            config.
                gravityMetersPerSecondSquared *
            std::max(
                southStar.h,
                0.0));

    const f64 signal =
        std::max(
            std::abs(vNorth) +
                waveNorth,
            std::abs(vSouth) +
                waveSouth);

    const Conserved numerical{
        .h =
            0.5 *
                (fNorth.h +
                 fSouth.h) -
            0.5 *
                signal *
                (southStar.h -
                 northStar.h),
        .qx =
            0.5 *
                (fNorth.qx +
                 fSouth.qx) -
            0.5 *
                signal *
                (southStar.qx -
                 northStar.qx),
        .qy =
            0.5 *
                (fNorth.qy +
                 fSouth.qy) -
            0.5 *
                signal *
                (southStar.qy -
                 northStar.qy)
    };

    const f64 northCorrection =
        0.5 *
        config.
            gravityMetersPerSecondSquared *
        (north.h *
             north.h -
         northStar.h *
             northStar.h);

    const f64 southCorrection =
        0.5 *
        config.
            gravityMetersPerSecondSquared *
        (south.h *
             south.h -
         southStar.h *
             southStar.h);

    return {
        .left = {
            .h =
                numerical.h,
            .qx =
                numerical.qx,
            .qy =
                numerical.qy +
                northCorrection
        },
        .right = {
            .h =
                numerical.h,
            .qx =
                numerical.qx,
            .qy =
                numerical.qy +
                southCorrection
        }
    };
}

[[nodiscard]] const std::vector<CoastalBoundaryCell>&
BoundarySide(
    const CoastalBoundaryState& boundary,
    const Side side)
{
    switch (side)
    {
    case Side::North:
        return boundary.north;
    case Side::East:
        return boundary.east;
    case Side::South:
        return boundary.south;
    case Side::West:
        return boundary.west;
    }

    return boundary.north;
}

[[nodiscard]] math::Double2 CellOffset(
    const u32 resolution,
    const f64 spacing,
    const u32 x,
    const u32 y) noexcept
{
    const f64 half =
        static_cast<f64>(
            resolution - 1U) *
        0.5;

    return {
        (static_cast<f64>(x) -
         half) *
            spacing,
        (static_cast<f64>(y) -
         half) *
            spacing
    };
}

[[nodiscard]] Conserved BoundaryGhost(
    const Side side,
    const u32 edgeIndex,
    const Conserved& source,
    const CoastalBoundaryState& boundary,
    const CoastalShallowWaterConfig& config,
    const u32 resolution,
    const f64 spacing,
    const f64 timeSeconds)
{
    const bool allClosed =
        boundary.north.empty() &&
        boundary.east.empty() &&
        boundary.south.empty() &&
        boundary.west.empty();

    if (allClosed)
    {
        Conserved ghost =
            source;

        if (side ==
                Side::East ||
            side ==
                Side::West)
        {
            ghost.qx =
                -ghost.qx;
        }
        else
        {
            ghost.qy =
                -ghost.qy;
        }

        return ghost;
    }

    const auto& cells =
        BoundarySide(
            boundary,
            side);

    const CoastalBoundaryCell& cell =
        cells[edgeIndex];

    if (cell.mode ==
        CoastalBoundaryMode::ClosedWall)
    {
        Conserved ghost =
            source;

        if (side ==
                Side::East ||
            side ==
                Side::West)
        {
            ghost.qx =
                -ghost.qx;
        }
        else
        {
            ghost.qy =
                -ghost.qy;
        }

        return ghost;
    }

    f64 surface =
        static_cast<f64>(
            cell.
                waterSurfaceElevationMeters);

    f64 velocityX =
        static_cast<f64>(
            cell.
                velocityEastMetersPerSecond);

    f64 velocityY =
        static_cast<f64>(
            cell.
                velocitySouthMetersPerSecond);

    if (cell.mode ==
            CoastalBoundaryMode::OpenOcean &&
        config.wave.enabled)
    {
        const math::Double2 direction =
            Normalize(
                config.wave.direction);

        u32 x = edgeIndex;
        u32 y = edgeIndex;

        switch (side)
        {
        case Side::North:
            y = 0U;
            break;
        case Side::East:
            x = resolution - 1U;
            break;
        case Side::South:
            y = resolution - 1U;
            break;
        case Side::West:
            x = 0U;
            break;
        }

        const math::Double2 offset =
            CellOffset(
                resolution,
                spacing,
                x,
                y);

        const f64 stillDepth =
            std::max(
                surface -
                    static_cast<f64>(
                        cell.
                            bedElevationMeters),
                config.
                    wetThresholdMeters);

        const f64 waveSpeed =
            std::sqrt(
                config.
                    gravityMetersPerSecondSquared *
                stillDepth);

        const f64 angularFrequency =
            2.0 *
            kPi /
            config.wave.
                periodSeconds;

        const f64 waveNumber =
            angularFrequency /
            std::max(
                waveSpeed,
                1.0e-6);

        const f64 phase =
            angularFrequency *
                timeSeconds -
            waveNumber *
                (direction.x *
                     offset.x +
                 direction.y *
                     offset.y) +
            config.wave.
                phaseRadians;

        const f64 eta =
            config.wave.
                amplitudeMeters *
            std::sin(
                phase);

        surface +=
            eta;

        const f64 linearVelocity =
            eta *
            std::sqrt(
                config.
                    gravityMetersPerSecondSquared /
                stillDepth);

        velocityX +=
            direction.x *
            linearVelocity;

        velocityY +=
            direction.y *
            linearVelocity;
    }

    const f64 bed =
        static_cast<f64>(
            cell.
                bedElevationMeters);

    const f64 depth =
        std::max(
            surface -
                bed,
            0.0);

    if (depth <=
        config.
            dryThresholdMeters)
    {
        return {};
    }

    return {
        .h =
            depth,
        .qx =
            depth *
            velocityX,
        .qy =
            depth *
            velocityY
    };
}

[[nodiscard]] f64 BoundaryBed(
    const Side side,
    const u32 edgeIndex,
    const f64 sourceBed,
    const CoastalBoundaryState& boundary) noexcept
{
    const bool allClosed =
        boundary.north.empty() &&
        boundary.east.empty() &&
        boundary.south.empty() &&
        boundary.west.empty();

    if (allClosed)
    {
        return sourceBed;
    }

    const auto& cells =
        BoundarySide(
            boundary,
            side);

    if (cells[edgeIndex].mode ==
        CoastalBoundaryMode::ClosedWall)
    {
        return sourceBed;
    }

    return
        static_cast<f64>(
            cells[edgeIndex].
                bedElevationMeters);
}

[[nodiscard]] CoastalBoundaryMode BoundaryModeAt(
    const Side side,
    const u32 edgeIndex,
    const CoastalBoundaryState& boundary) noexcept
{
    const bool allClosed =
        boundary.north.empty() &&
        boundary.east.empty() &&
        boundary.south.empty() &&
        boundary.west.empty();

    if (allClosed)
    {
        return
            CoastalBoundaryMode::
                ClosedWall;
    }

    return
        BoundarySide(
            boundary,
            side)[edgeIndex].
            mode;
}

void RefreshDerivedState(
    CoastalWaterPage& page,
    const terrain_material_column::MaterialColumnPage& material,
    const CoastalShallowWaterConfig& config)
{
    const u32 resolution =
        page.resolution;

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            auto& state =
                page.At(
                    x,
                    y);

            const f64 bed =
                SurfaceHeight(
                    material,
                    x,
                    y);

            if (state.waterDepthMeters <=
                config.
                    dryThresholdMeters)
            {
                state.waterDepthMeters =
                    0.0;

                state.
                    momentumEastSquareMetersPerSecond =
                        0.0;

                state.
                    momentumSouthSquareMetersPerSecond =
                        0.0;
            }

            state.bedElevationMeters =
                bed;

            state.wet =
                state.
                    waterDepthMeters >=
                config.
                    wetThresholdMeters;

            state.
                waterSurfaceElevationMeters =
                    bed +
                    state.
                        waterDepthMeters;

            if (state.wet)
            {
                state.
                    velocityMetersPerSecond = {
                        state.
                            momentumEastSquareMetersPerSecond /
                            state.
                                waterDepthMeters,
                        state.
                            momentumSouthSquareMetersPerSecond /
                            state.
                                waterDepthMeters
                    };
            }
            else
            {
                state.
                    velocityMetersPerSecond = {};
            }

            const f64 speed =
                Length(
                    state.
                        velocityMetersPerSecond);

            state.
                specificWaveCurrentEnergySquareMetersPerSecondSquared =
                    0.5 *
                        speed *
                        speed +
                    config.
                        gravityMetersPerSecondSquared *
                        std::abs(
                            state.
                                waterSurfaceElevationMeters -
                            config.
                                seaLevelMeters);

            state.shoreline = false;
        }
    }

    constexpr std::array<
        std::pair<i32, i32>,
        4>
        offsets{{
            {-1, 0},
            {1, 0},
            {0, -1},
            {0, 1}
        }};

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            auto& source =
                page.At(
                    x,
                    y);

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

                const bool neighborWet =
                    page.
                        At(
                            static_cast<u32>(nx),
                            static_cast<u32>(ny)).
                        wet;

                if (neighborWet !=
                    source.wet)
                {
                    source.shoreline =
                        true;
                    break;
                }
            }
        }
    }
}

[[nodiscard]] f64 StableTimeStep(
    const CoastalWaterPage& page,
    const CoastalBoundaryState& boundary,
    const CoastalShallowWaterConfig& config) noexcept
{
    f64 maximumSignal = 0.0;

    for (const auto& state :
         page.cells)
    {
        const f64 wave =
            std::sqrt(
                config.
                    gravityMetersPerSecondSquared *
                std::max(
                    state.
                        waterDepthMeters,
                    0.0));

        maximumSignal =
            std::max(
                maximumSignal,
                std::abs(
                    state.
                        velocityMetersPerSecond.x) +
                    wave);

        maximumSignal =
            std::max(
                maximumSignal,
                std::abs(
                    state.
                        velocityMetersPerSecond.y) +
                    wave);
    }

    const auto includeBoundary =
        [&](const std::vector<CoastalBoundaryCell>& side)
        {
            for (const auto& cell :
                 side)
            {
                if (cell.mode ==
                    CoastalBoundaryMode::ClosedWall)
                {
                    continue;
                }

                const f64 depth =
                    std::max(
                        static_cast<f64>(
                            cell.
                                waterSurfaceElevationMeters -
                            cell.
                                bedElevationMeters),
                        0.0);

                const f64 wave =
                    std::sqrt(
                        config.
                            gravityMetersPerSecondSquared *
                        depth);

                maximumSignal =
                    std::max(
                        maximumSignal,
                        std::abs(
                            static_cast<f64>(
                                cell.
                                    velocityEastMetersPerSecond)) +
                            wave +
                            2.0 *
                                std::abs(
                                    config.wave.
                                        amplitudeMeters) /
                                std::max(
                                    config.wave.
                                        periodSeconds,
                                    1.0e-6));

                maximumSignal =
                    std::max(
                        maximumSignal,
                        std::abs(
                            static_cast<f64>(
                                cell.
                                    velocitySouthMetersPerSecond)) +
                            wave +
                            2.0 *
                                std::abs(
                                    config.wave.
                                        amplitudeMeters) /
                                std::max(
                                    config.wave.
                                        periodSeconds,
                                    1.0e-6));
            }
        };

    includeBoundary(
        boundary.north);

    includeBoundary(
        boundary.east);

    includeBoundary(
        boundary.south);

    includeBoundary(
        boundary.west);

    if (maximumSignal <=
        1.0e-12)
    {
        return
            config.
                maximumTimeStepSeconds;
    }

    return
        std::min(
            config.
                maximumTimeStepSeconds,
            config.
                cflNumber *
                page.
                    spacingMeters /
                maximumSignal);
}

[[nodiscard]] f64 ExposedCoastalMobility(
    const terrain_material_column::MaterialColumnPage& material,
    const terrain_geology::GeologicalMaterialLibrary& geology,
    const u32 x,
    const u32 y)
{
    const auto& cell =
        material.At(
            x,
            y);

    using Surface =
        terrain_material_column::
            ExposedSurfaceKind;

    switch (cell.ExposedSurface())
    {
    case Surface::Sand:
        return 1.0;
    case Surface::Soil:
        return 0.70;
    case Surface::Regolith:
        return 0.48;
    case Surface::Debris:
        return 0.36;
    case Surface::Bedrock:
    {
        const auto* rock =
            geology.Find(
                cell.
                    bedrockMaterial);

        if (rock == nullptr)
        {
            throw std::invalid_argument(
                "Orbit M17 encountered unknown M02 coastal bedrock.");
        }

        const f64 hydraulic =
            std::clamp(
                static_cast<f64>(
                    rock->
                        hydraulicErodibility),
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

        const f64 fracture =
            std::clamp(
                static_cast<f64>(
                    rock->
                        fractureTendency),
                0.0,
                1.0);

        return
            std::clamp(
                (0.15 +
                 0.85 *
                     hydraulic) *
                (1.0 -
                 0.60 *
                     hardness) *
                (1.0 -
                 0.30 *
                     cohesion) *
                (0.70 +
                 0.30 *
                     fracture),
                0.01,
                1.0);
    }
    }

    return 0.0;
}

[[nodiscard]] terrain_erosion::SedimentMass ScaleSediment(
    const terrain_erosion::SedimentMass& mass,
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

void MoveToBedload(
    terrain_erosion::SedimentExchangePage& sediment,
    const u32 x,
    const u32 y,
    const terrain_erosion::SedimentMass& picked,
    const CoastalSedimentConfig& config)
{
    const terrain_erosion::SedimentMass requested{
        .sandKg =
            picked.sandKg *
            config.
                sandBedloadFraction,
        .finesKg =
            picked.finesKg *
            config.
                finesBedloadFraction,
        .coarseDebrisKg =
            picked.
                coarseDebrisKg *
            config.
                coarseDebrisBedloadFraction
    };

    const auto moved =
        sediment.Take(
            x,
            y,
            terrain_erosion::
                SedimentTransportMedium::
                    Waterborne,
            requested);

    sediment.Add(
        x,
        y,
        terrain_erosion::
            SedimentTransportMedium::
                SurfaceMobile,
        moved);
}

struct TransportScratch
{
    std::vector<terrain_erosion::SedimentMass>
        waterborne;

    std::vector<terrain_erosion::SedimentMass>
        surface;
};

[[nodiscard]] std::pair<i32, i32>
TransportTarget(
    const u32 x,
    const u32 y,
    const bool xAxis,
    const f64 component) noexcept
{
    if (xAxis)
    {
        return {
            static_cast<i32>(x) +
                (component >= 0.0
                     ? 1
                     : -1),
            static_cast<i32>(y)
        };
    }

    return {
        static_cast<i32>(x),
        static_cast<i32>(y) +
            (component >= 0.0
                 ? 1
                 : -1)
    };
}

void AdvectSediment(
    terrain_erosion::SedimentExchangePage& sediment,
    const CoastalWaterPage& water,
    const CoastalSedimentConfig& config,
    CoastalProcessDiagnostics& diagnostics)
{
    const u32 resolution =
        water.resolution;

    const std::size_t count =
        static_cast<std::size_t>(
            resolution) *
        resolution;

    std::vector<
        terrain_erosion::SedimentMass>
        waterSnapshot(
            count);

    std::vector<
        terrain_erosion::SedimentMass>
        surfaceSnapshot(
            count);

    TransportScratch next{
        .waterborne =
            std::vector<
                terrain_erosion::SedimentMass>(
                    count),
        .surface =
            std::vector<
                terrain_erosion::SedimentMass>(
                    count)
    };

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

            const auto& cell =
                sediment.At(
                    x,
                    y);

            waterSnapshot[index] =
                cell.waterborne;

            surfaceSnapshot[index] =
                cell.surfaceMobile;
        }
    }

    const f64 dt =
        water.
            lastTimeStepSeconds;

    const auto transportMedium =
        [&](const u32 x,
            const u32 y,
            const terrain_erosion::
                SedimentTransportMedium medium,
            const terrain_erosion::SedimentMass& source,
            std::vector<terrain_erosion::SedimentMass>& destination,
            f64& diagnostic)
        {
            const std::size_t index =
                Index(
                    resolution,
                    x,
                    y);

            const auto& state =
                water.cells[index];

            if (!state.wet ||
                source.Empty())
            {
                destination[index] +=
                    source;
                return;
            }

            const f64 vx =
                state.
                    velocityMetersPerSecond.x;

            const f64 vy =
                state.
                    velocityMetersPerSecond.y;

            const f64 speed =
                std::hypot(
                    vx,
                    vy);

            const f64 fraction =
                std::clamp(
                    config.transportRate *
                        speed *
                        dt /
                        std::max(
                            water.
                                spacingMeters,
                            1.0e-9),
                    0.0,
                    config.
                        maximumTransportFractionPerStep);

            if (fraction <= 0.0)
            {
                destination[index] +=
                    source;
                return;
            }

            const f64 sum =
                std::abs(vx) +
                std::abs(vy);

            if (sum <= 1.0e-12)
            {
                destination[index] +=
                    source;
                return;
            }

            const f64 fractionX =
                fraction *
                std::abs(vx) /
                sum;

            const f64 fractionY =
                fraction *
                std::abs(vy) /
                sum;

            const f64 retained =
                std::max(
                    1.0 -
                        fractionX -
                        fractionY,
                    0.0);

            destination[index] +=
                ScaleSediment(
                    source,
                    retained);

            const auto dispatch =
                [&](const bool xAxis,
                    const f64 component,
                    const f64 componentFraction)
                {
                    if (componentFraction <=
                        0.0)
                    {
                        return;
                    }

                    const auto [tx, ty] =
                        TransportTarget(
                            x,
                            y,
                            xAxis,
                            component);

                    const auto moved =
                        ScaleSediment(
                            source,
                            componentFraction);

                    diagnostic +=
                        moved.TotalKg();

                    if (Inside(
                            tx,
                            ty,
                            resolution))
                    {
                        destination[
                            Index(
                                resolution,
                                static_cast<u32>(tx),
                                static_cast<u32>(ty))] +=
                                    moved;

                        sediment.RecordTransport(
                            x,
                            y,
                            tx,
                            ty,
                            medium,
                            moved);

                        return;
                    }

                    static_cast<void>(
                        sediment.
                            ExportAcrossBoundary(
                                x,
                                y,
                                tx,
                                ty,
                                medium,
                                moved));
                };

            dispatch(
                true,
                vx,
                fractionX);

            dispatch(
                false,
                vy,
                fractionY);
        };

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

            transportMedium(
                x,
                y,
                terrain_erosion::
                    SedimentTransportMedium::
                        Waterborne,
                waterSnapshot[index],
                next.waterborne,
                diagnostics.
                    transportedWaterborneKg);

            transportMedium(
                x,
                y,
                terrain_erosion::
                    SedimentTransportMedium::
                        SurfaceMobile,
                surfaceSnapshot[index],
                next.surface,
                diagnostics.
                    transportedBedloadKg);
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

            auto& cell =
                sediment.At(
                    x,
                    y);

            cell.waterborne =
                next.waterborne[index];

            cell.surfaceMobile =
                next.surface[index];
        }
    }
}

void DepositCoastalSediment(
    terrain_material_column::MaterialColumnPage& material,
    terrain_erosion::SedimentExchangePage& sediment,
    const CoastalWaterPage& water,
    const CoastalSedimentConfig& config,
    CoastalProcessDiagnostics& diagnostics)
{
    const u32 resolution =
        water.resolution;

    const f64 dt =
        water.
            lastTimeStepSeconds;

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const auto& waterCell =
                water.At(
                    x,
                    y);

            const f64 speed =
                Length(
                    waterCell.
                        velocityMetersPerSecond);

            const f64 lowEnergy =
                std::clamp(
                    1.0 -
                        speed /
                            std::max(
                                config.
                                    depositionVelocityThresholdMetersPerSecond,
                                1.0e-9),
                    0.0,
                    1.0);

            f64 multiplier = 1.0;

            if (waterCell.shoreline)
            {
                multiplier =
                    config.
                        shorelineDepositionMultiplier;
            }

            const f64 surfaceFraction =
                std::clamp(
                    config.
                        depositionRatePerSecond *
                        dt *
                        lowEnergy *
                        multiplier,
                    0.0,
                    1.0);

            const f64 suspendedFraction =
                std::clamp(
                    0.5 *
                        config.
                            depositionRatePerSecond *
                        dt *
                        lowEnergy,
                    0.0,
                    1.0);

            if (surfaceFraction >
                0.0)
            {
                const f64 available =
                    sediment.
                        At(x, y).
                        surfaceMobile.
                        TotalKg();

                if (available > 0.0)
                {
                    const auto deposited =
                        sediment.
                            DepositToColumn(
                                material,
                                x,
                                y,
                                terrain_erosion::
                                    SedimentTransportMedium::
                                        SurfaceMobile,
                                available *
                                    surfaceFraction);

                    diagnostics.
                        depositedMassKg +=
                            deposited.
                                DepositedKg();
                }
            }

            if (suspendedFraction >
                0.0)
            {
                const f64 available =
                    sediment.
                        At(x, y).
                        waterborne.
                        TotalKg();

                if (available > 0.0)
                {
                    const auto deposited =
                        sediment.
                            DepositToColumn(
                                material,
                                x,
                                y,
                                terrain_erosion::
                                    SedimentTransportMedium::
                                        Waterborne,
                                available *
                                    suspendedFraction);

                    diagnostics.
                        depositedMassKg +=
                            deposited.
                                DepositedKg();
                }
            }
        }
    }
}

void SynchronizeWaterAfterBedChange(
    CoastalWaterPage& water,
    const terrain_material_column::MaterialColumnPage& material,
    const std::span<const f64> oldBed,
    const CoastalShallowWaterConfig& config)
{
    const u32 resolution =
        water.resolution;

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
                water.cells[index];

            const f64 oldSurface =
                oldBed[index] +
                state.
                    waterDepthMeters;

            const f64 newBed =
                SurfaceHeight(
                    material,
                    x,
                    y);

            state.waterDepthMeters =
                std::max(
                    oldSurface -
                        newBed,
                    0.0);

            state.bedElevationMeters =
                newBed;
        }
    }

    RefreshDerivedState(
        water,
        material,
        config);
}
} // namespace

bool CoastalBoundaryCell::IsValid() const noexcept
{
    return
        std::isfinite(
            bedElevationMeters) &&
        std::isfinite(
            waterSurfaceElevationMeters) &&
        std::isfinite(
            velocityEastMetersPerSecond) &&
        std::isfinite(
            velocitySouthMetersPerSecond);
}

bool CoastalBoundaryState::IsComplete(
    const u32 resolution) const noexcept
{
    if (resolution == 0U)
    {
        return false;
    }

    const bool allEmpty =
        north.empty() &&
        east.empty() &&
        south.empty() &&
        west.empty();

    if (allEmpty)
    {
        return true;
    }

    return
        north.size() ==
            resolution &&
        east.size() ==
            resolution &&
        south.size() ==
            resolution &&
        west.size() ==
            resolution &&
        std::all_of(
            north.begin(),
            north.end(),
            [](const CoastalBoundaryCell& cell)
            {
                return cell.IsValid();
            }) &&
        std::all_of(
            east.begin(),
            east.end(),
            [](const CoastalBoundaryCell& cell)
            {
                return cell.IsValid();
            }) &&
        std::all_of(
            south.begin(),
            south.end(),
            [](const CoastalBoundaryCell& cell)
            {
                return cell.IsValid();
            }) &&
        std::all_of(
            west.begin(),
            west.end(),
            [](const CoastalBoundaryCell& cell)
            {
                return cell.IsValid();
            });
}

bool CoastalWaveForcing::IsValid() const noexcept
{
    return
        std::isfinite(
            amplitudeMeters) &&
        amplitudeMeters >= 0.0 &&
        std::isfinite(
            periodSeconds) &&
        periodSeconds > 0.0 &&
        std::isfinite(
            phaseRadians) &&
        std::isfinite(
            direction.x) &&
        std::isfinite(
            direction.y) &&
        (!enabled ||
         Length(
             direction) >
             1.0e-12);
}

bool CoastalShallowWaterConfig::IsValid() const noexcept
{
    return
        std::isfinite(
            seaLevelMeters) &&
        std::isfinite(
            gravityMetersPerSecondSquared) &&
        gravityMetersPerSecondSquared >
            0.0 &&
        std::isfinite(
            cflNumber) &&
        cflNumber > 0.0 &&
        cflNumber <= 0.5 &&
        std::isfinite(
            maximumTimeStepSeconds) &&
        maximumTimeStepSeconds >
            0.0 &&
        std::isfinite(
            wetThresholdMeters) &&
        wetThresholdMeters > 0.0 &&
        std::isfinite(
            dryThresholdMeters) &&
        dryThresholdMeters >= 0.0 &&
        dryThresholdMeters <
            wetThresholdMeters &&
        std::isfinite(
            manningRoughness) &&
        manningRoughness >= 0.0 &&
        std::isfinite(
            maximumVelocityMetersPerSecond) &&
        maximumVelocityMetersPerSecond >
            0.0 &&
        wave.IsValid();
}

CoastalCellState&
CoastalWaterPage::At(
    const u32 x,
    const u32 y)
{
    if (x >= resolution ||
        y >= resolution)
    {
        throw std::out_of_range(
            "Orbit M17 coastal water coordinate is out of range.");
    }

    return
        cells[
            Index(
                resolution,
                x,
                y)];
}

const CoastalCellState&
CoastalWaterPage::At(
    const u32 x,
    const u32 y) const
{
    if (x >= resolution ||
        y >= resolution)
    {
        throw std::out_of_range(
            "Orbit M17 coastal water coordinate is out of range.");
    }

    return
        cells[
            Index(
                resolution,
                x,
                y)];
}

CoastalWaterPage InitializeCoastalShallowWater(
    const terrain_material_column::MaterialColumnPage& material,
    const CoastalShallowWaterConfig& config)
{
    if (!config.IsValid())
    {
        throw std::invalid_argument(
            "Orbit M17 shallow-water configuration is invalid.");
    }

    CoastalWaterPage result{};
    result.resolution =
        material.Resolution();

    result.spacingMeters =
        material.
            SpacingMeters();

    result.cells.resize(
        static_cast<std::size_t>(
            result.resolution) *
        result.resolution);

    for (u32 y = 0U;
         y < result.resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < result.resolution;
             ++x)
        {
            const f64 bed =
                SurfaceHeight(
                    material,
                    x,
                    y);

            auto& state =
                result.At(
                    x,
                    y);

            state.bedElevationMeters =
                bed;

            state.waterDepthMeters =
                std::max(
                    config.
                        seaLevelMeters -
                        bed,
                    0.0);
        }
    }

    RefreshDerivedState(
        result,
        material,
        config);

    result.balance.
        initialVolumeCubicMeters =
            TotalWaterVolume(
                result);

    result.balance.
        finalVolumeCubicMeters =
            result.balance.
                initialVolumeCubicMeters;

    return result;
}

void AdvanceCoastalShallowWater(
    CoastalWaterPage& water,
    const terrain_material_column::MaterialColumnPage& material,
    const CoastalBoundaryState& boundary,
    const CoastalShallowWaterConfig& config,
    const u32 stepCount)
{
    if (!config.IsValid() ||
        water.resolution == 0U ||
        water.resolution !=
            material.Resolution() ||
        std::abs(
            water.spacingMeters -
            material.
                SpacingMeters()) >
            1.0e-9 ||
        water.cells.size() !=
            static_cast<std::size_t>(
                water.resolution) *
                water.resolution ||
        !boundary.IsComplete(
            water.resolution))
    {
        throw std::invalid_argument(
            "Orbit M17 shallow-water page/boundary input is invalid.");
    }

    const u32 resolution =
        water.resolution;

    std::vector<Conserved> next(
        water.cells.size());

    for (u32 step = 0U;
         step < stepCount;
         ++step)
    {
        // M08 may have changed since the previous water step. Preserve each
        // cell's free-surface elevation while reconciling depth to the new bed.
        for (u32 y = 0U;
             y < resolution;
             ++y)
        {
            for (u32 x = 0U;
                 x < resolution;
                 ++x)
            {
                auto& state =
                    water.At(
                        x,
                        y);

                const f64 newBed =
                    SurfaceHeight(
                        material,
                        x,
                        y);

                const f64 previousSurface =
                    state.
                        bedElevationMeters +
                    state.
                        waterDepthMeters;

                state.waterDepthMeters =
                    std::max(
                        previousSurface -
                            newBed,
                        0.0);

                state.bedElevationMeters =
                    newBed;
            }
        }

        RefreshDerivedState(
            water,
            material,
            config);

        const f64 dt =
            StableTimeStep(
                water,
                boundary,
                config);

        water.lastTimeStepSeconds =
            dt;

        f64 boundaryVolume =
            0.0;

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

                const auto& sourceState =
                    water.cells[index];

                const f64 bed =
                    SurfaceHeight(
                        material,
                        x,
                        y);

                const Conserved source{
                    .h =
                        sourceState.
                            waterDepthMeters,
                    .qx =
                        sourceState.
                            momentumEastSquareMetersPerSecond,
                    .qy =
                        sourceState.
                            momentumSouthSquareMetersPerSecond
                };

                Conserved westState{};
                f64 westBed = bed;

                if (x > 0U)
                {
                    const auto& neighbor =
                        water.At(
                            x - 1U,
                            y);

                    westState = {
                        .h =
                            neighbor.
                                waterDepthMeters,
                        .qx =
                            neighbor.
                                momentumEastSquareMetersPerSecond,
                        .qy =
                            neighbor.
                                momentumSouthSquareMetersPerSecond
                    };

                    westBed =
                        SurfaceHeight(
                            material,
                            x - 1U,
                            y);
                }
                else
                {
                    westState =
                        BoundaryGhost(
                            Side::West,
                            y,
                            source,
                            boundary,
                            config,
                            resolution,
                            water.spacingMeters,
                            water.elapsedSeconds);

                    westBed =
                        BoundaryBed(
                            Side::West,
                            y,
                            bed,
                            boundary);
                }

                Conserved eastState{};
                f64 eastBed = bed;

                if (x + 1U <
                    resolution)
                {
                    const auto& neighbor =
                        water.At(
                            x + 1U,
                            y);

                    eastState = {
                        .h =
                            neighbor.
                                waterDepthMeters,
                        .qx =
                            neighbor.
                                momentumEastSquareMetersPerSecond,
                        .qy =
                            neighbor.
                                momentumSouthSquareMetersPerSecond
                    };

                    eastBed =
                        SurfaceHeight(
                            material,
                            x + 1U,
                            y);
                }
                else
                {
                    eastState =
                        BoundaryGhost(
                            Side::East,
                            y,
                            source,
                            boundary,
                            config,
                            resolution,
                            water.spacingMeters,
                            water.elapsedSeconds);

                    eastBed =
                        BoundaryBed(
                            Side::East,
                            y,
                            bed,
                            boundary);
                }

                Conserved northState{};
                f64 northBed = bed;

                if (y > 0U)
                {
                    const auto& neighbor =
                        water.At(
                            x,
                            y - 1U);

                    northState = {
                        .h =
                            neighbor.
                                waterDepthMeters,
                        .qx =
                            neighbor.
                                momentumEastSquareMetersPerSecond,
                        .qy =
                            neighbor.
                                momentumSouthSquareMetersPerSecond
                    };

                    northBed =
                        SurfaceHeight(
                            material,
                            x,
                            y - 1U);
                }
                else
                {
                    northState =
                        BoundaryGhost(
                            Side::North,
                            x,
                            source,
                            boundary,
                            config,
                            resolution,
                            water.spacingMeters,
                            water.elapsedSeconds);

                    northBed =
                        BoundaryBed(
                            Side::North,
                            x,
                            bed,
                            boundary);
                }

                Conserved southState{};
                f64 southBed = bed;

                if (y + 1U <
                    resolution)
                {
                    const auto& neighbor =
                        water.At(
                            x,
                            y + 1U);

                    southState = {
                        .h =
                            neighbor.
                                waterDepthMeters,
                        .qx =
                            neighbor.
                                momentumEastSquareMetersPerSecond,
                        .qy =
                            neighbor.
                                momentumSouthSquareMetersPerSecond
                    };

                    southBed =
                        SurfaceHeight(
                            material,
                            x,
                            y + 1U);
                }
                else
                {
                    southState =
                        BoundaryGhost(
                            Side::South,
                            x,
                            source,
                            boundary,
                            config,
                            resolution,
                            water.spacingMeters,
                            water.elapsedSeconds);

                    southBed =
                        BoundaryBed(
                            Side::South,
                            x,
                            bed,
                            boundary);
                }

                const InterfaceFlux west =
                    FluxX(
                        westState,
                        westBed,
                        source,
                        bed,
                        config);

                const InterfaceFlux east =
                    FluxX(
                        source,
                        bed,
                        eastState,
                        eastBed,
                        config);

                const InterfaceFlux north =
                    FluxY(
                        northState,
                        northBed,
                        source,
                        bed,
                        config);

                const InterfaceFlux south =
                    FluxY(
                        source,
                        bed,
                        southState,
                        southBed,
                        config);

                Conserved updated{
                    .h =
                        source.h -
                        dt /
                            water.
                                spacingMeters *
                            ((east.left.h -
                              west.right.h) +
                             (south.left.h -
                              north.right.h)),
                    .qx =
                        source.qx -
                        dt /
                            water.
                                spacingMeters *
                            ((east.left.qx -
                              west.right.qx) +
                             (south.left.qx -
                              north.right.qx)),
                    .qy =
                        source.qy -
                        dt /
                            water.
                                spacingMeters *
                            ((east.left.qy -
                              west.right.qy) +
                             (south.left.qy -
                              north.right.qy))
                };

                if (updated.h <
                    config.
                        dryThresholdMeters)
                {
                    updated = {};
                }
                else
                {
                    updated.h =
                        std::max(
                            updated.h,
                            0.0);

                    f64 velocityX =
                        updated.qx /
                        updated.h;

                    f64 velocityY =
                        updated.qy /
                        updated.h;

                    const f64 speed =
                        std::hypot(
                            velocityX,
                            velocityY);

                    if (speed >
                            config.
                                maximumVelocityMetersPerSecond &&
                        speed > 0.0)
                    {
                        const f64 scale =
                            config.
                                maximumVelocityMetersPerSecond /
                            speed;

                        velocityX *= scale;
                        velocityY *= scale;

                        updated.qx =
                            velocityX *
                            updated.h;

                        updated.qy =
                            velocityY *
                            updated.h;
                    }

                    if (config.
                            manningRoughness >
                        0.0)
                    {
                        const f64 currentSpeed =
                            std::hypot(
                                velocityX,
                                velocityY);

                        const f64 depthTerm =
                            std::pow(
                                std::max(
                                    updated.h,
                                    config.
                                        wetThresholdMeters),
                                4.0 / 3.0);

                        const f64 damping =
                            1.0 /
                            (1.0 +
                             dt *
                                 config.
                                     gravityMetersPerSecondSquared *
                                 config.
                                     manningRoughness *
                                 config.
                                     manningRoughness *
                                 currentSpeed /
                                 std::max(
                                     depthTerm,
                                     1.0e-9));

                        updated.qx *=
                            damping;

                        updated.qy *=
                            damping;
                    }
                }

                next[index] =
                    updated;

                if (x == 0U &&
                    BoundaryModeAt(
                        Side::West,
                        y,
                        boundary) !=
                        CoastalBoundaryMode::ClosedWall)
                {
                    boundaryVolume +=
                        dt *
                        west.right.h *
                        water.
                            spacingMeters;
                }

                if (x + 1U ==
                        resolution &&
                    BoundaryModeAt(
                        Side::East,
                        y,
                        boundary) !=
                        CoastalBoundaryMode::ClosedWall)
                {
                    boundaryVolume -=
                        dt *
                        east.left.h *
                        water.
                            spacingMeters;
                }

                if (y == 0U &&
                    BoundaryModeAt(
                        Side::North,
                        x,
                        boundary) !=
                        CoastalBoundaryMode::ClosedWall)
                {
                    boundaryVolume +=
                        dt *
                        north.right.h *
                        water.
                            spacingMeters;
                }

                if (y + 1U ==
                        resolution &&
                    BoundaryModeAt(
                        Side::South,
                        x,
                        boundary) !=
                        CoastalBoundaryMode::ClosedWall)
                {
                    boundaryVolume -=
                        dt *
                        south.left.h *
                        water.
                            spacingMeters;
                }
            }
        }

        for (std::size_t index = 0U;
             index <
                 water.cells.size();
             ++index)
        {
            auto& state =
                water.cells[index];

            state.waterDepthMeters =
                next[index].h;

            state.
                momentumEastSquareMetersPerSecond =
                    next[index].qx;

            state.
                momentumSouthSquareMetersPerSecond =
                    next[index].qy;
        }

        water.elapsedSeconds +=
            dt;

        water.balance.
            cumulativeBoundaryVolumeCubicMeters +=
                boundaryVolume;
    }

    RefreshDerivedState(
        water,
        material,
        config);

    water.balance.
        finalVolumeCubicMeters =
            TotalWaterVolume(
                water);

    water.balance.
        balanceErrorCubicMeters =
            water.balance.
                initialVolumeCubicMeters +
            water.balance.
                cumulativeBoundaryVolumeCubicMeters -
            water.balance.
                finalVolumeCubicMeters;

    const f64 reference =
        std::max(
            std::abs(
                water.balance.
                    initialVolumeCubicMeters) +
                std::abs(
                    water.balance.
                        cumulativeBoundaryVolumeCubicMeters),
            1.0);

    water.balance.
        balanceRelativeError =
            std::abs(
                water.balance.
                    balanceErrorCubicMeters) /
            reference;
}

bool CoastalCellForcing::IsValid() const noexcept
{
    return
        std::isfinite(
            protection) &&
        protection >= 0.0F &&
        protection <= 1.0F;
}

bool CoastalSedimentConfig::IsValid() const noexcept
{
    const auto nonnegative =
        [](const f64 value)
        {
            return
                std::isfinite(value) &&
                value >= 0.0;
        };

    const auto unit =
        [](const f64 value)
        {
            return
                std::isfinite(value) &&
                value >= 0.0 &&
                value <= 1.0;
        };

    return
        std::isfinite(
            activeDepthMeters) &&
        activeDepthMeters > 0.0 &&
        std::isfinite(
            referenceEnergySquareMetersPerSecondSquared) &&
        referenceEnergySquareMetersPerSecondSquared > 0.0 &&
        nonnegative(
            erosionMetersPerSecondAtReference) &&
        nonnegative(
            maximumErosionDepthPerStepMeters) &&
        unit(
            sandBedloadFraction) &&
        unit(
            finesBedloadFraction) &&
        unit(
            coarseDebrisBedloadFraction) &&
        nonnegative(
            transportRate) &&
        unit(
            maximumTransportFractionPerStep) &&
        nonnegative(
            depositionVelocityThresholdMetersPerSecond) &&
        nonnegative(
            depositionRatePerSecond) &&
        nonnegative(
            shorelineDepositionMultiplier);
}

bool CoastalProcessConfig::IsValid() const noexcept
{
    return
        hydrodynamicSteps > 0U &&
        water.IsValid() &&
        sediment.IsValid();
}

CoastalProcessResult SimulateCoastalProcess(
    terrain_material_column::MaterialColumnPage material,
    const terrain_geology::GeologicalMaterialLibrary& geology,
    terrain_erosion::SedimentExchangePage sedimentExchange,
    const CoastalBoundaryState& boundary,
    const std::span<const CoastalCellForcing> forcing,
    const CoastalProcessConfig& config)
{
    if (!config.IsValid() ||
        material.Resolution() !=
            sedimentExchange.
                Resolution() ||
        std::abs(
            material.
                SpacingMeters() -
            sedimentExchange.
                SpacingMeters()) >
            1.0e-9 ||
        !boundary.IsComplete(
            material.
                Resolution()))
    {
        throw std::invalid_argument(
            "Orbit M17 coastal process inputs are invalid.");
    }

    const std::size_t cellCount =
        static_cast<std::size_t>(
            material.
                Resolution()) *
        material.
            Resolution();

    if (!forcing.empty() &&
        forcing.size() !=
            cellCount)
    {
        throw std::invalid_argument(
            "Orbit M17 coastal forcing must match the physical page.");
    }

    for (const auto& cell :
         forcing)
    {
        if (!cell.IsValid())
        {
            throw std::invalid_argument(
                "Orbit M17 coastal forcing contains an invalid cell.");
        }
    }

    const auto initialMaterial =
        material.QueryMass(
            geology);

    const f64 initialMobile =
        sedimentExchange.
            TotalMobileMass().
            TotalKg();

    const f64 initialExported =
        sedimentExchange.
            Accounting().
            exported.
            TotalKg();

    CoastalProcessResult result{
        .material =
            std::move(
                material),
        .sedimentExchange =
            std::move(
                sedimentExchange)
    };

    result.diagnostics.
        initialMaterialLooseKg =
            initialMaterial.
                LooseMassKg();

    result.diagnostics.
        initialMobileSedimentKg =
            initialMobile;

    if (!config.enabled)
    {
        result.water = {};

        result.diagnostics.
            finalMaterialLooseKg =
                initialMaterial.
                    LooseMassKg();

        result.diagnostics.
            finalMobileSedimentKg =
                initialMobile;

        return result;
    }

    result.water =
        InitializeCoastalShallowWater(
            result.material,
            config.water);

    const u32 resolution =
        result.material.
            Resolution();

    std::vector<f64> oldBed(
        cellCount,
        0.0);

    for (u32 step = 0U;
         step <
             config.
                 hydrodynamicSteps;
         ++step)
    {
        AdvanceCoastalShallowWater(
            result.water,
            result.material,
            boundary,
            config.water,
            1U);

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

                oldBed[index] =
                    SurfaceHeight(
                        result.material,
                        x,
                        y);
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

                const auto& waterCell =
                    result.water.
                        cells[index];

                if (!waterCell.wet ||
                    waterCell.
                        waterDepthMeters >
                    config.sediment.
                        activeDepthMeters)
                {
                    continue;
                }

                const f64 depthFactor =
                    std::clamp(
                        1.0 -
                            waterCell.
                                waterDepthMeters /
                                config.
                                    sediment.
                                    activeDepthMeters,
                        0.0,
                        1.0);

                const f64 shorelineFactor =
                    waterCell.shoreline
                        ? 1.0
                        : depthFactor;

                const f64 energyFactor =
                    std::clamp(
                        waterCell.
                            specificWaveCurrentEnergySquareMetersPerSecondSquared /
                            config.
                                sediment.
                                referenceEnergySquareMetersPerSecondSquared,
                        0.0,
                        6.0);

                if (energyFactor <=
                    0.0)
                {
                    continue;
                }

                const f64 protection =
                    forcing.empty()
                        ? 0.0
                        : std::clamp(
                            static_cast<f64>(
                                forcing[index].
                                    protection),
                            0.0,
                            1.0);

                const f64 mobility =
                    ExposedCoastalMobility(
                        result.material,
                        geology,
                        x,
                        y);

                const f64 erosionDepth =
                    std::min(
                        config.
                            sediment.
                            maximumErosionDepthPerStepMeters,
                        config.
                            sediment.
                            erosionMetersPerSecondAtReference *
                            result.water.
                                lastTimeStepSeconds *
                            energyFactor *
                            shorelineFactor *
                            mobility *
                            (1.0 -
                             protection));

                if (erosionDepth <=
                    0.0)
                {
                    continue;
                }

                const auto picked =
                    result.
                        sedimentExchange.
                        PickupFromColumn(
                            result.material,
                            geology,
                            x,
                            y,
                            erosionDepth,
                            terrain_erosion::
                                SedimentSourceProcess::
                                    CoastalErosion,
                            terrain_erosion::
                                SedimentTransportMedium::
                                    Waterborne);

                result.diagnostics.
                    erodedMassKg +=
                        picked.
                            TotalKg();

                MoveToBedload(
                    result.
                        sedimentExchange,
                    x,
                    y,
                    picked,
                    config.
                        sediment);
            }
        }

        AdvectSediment(
            result.
                sedimentExchange,
            result.water,
            config.sediment,
            result.
                diagnostics);

        DepositCoastalSediment(
            result.material,
            result.
                sedimentExchange,
            result.water,
            config.sediment,
            result.
                diagnostics);

        SynchronizeWaterAfterBedChange(
            result.water,
            result.material,
            oldBed,
            config.water);
    }

    u32 shorelineCount = 0U;

    for (const auto& cell :
         result.water.cells)
    {
        if (cell.shoreline)
        {
            ++shorelineCount;
        }
    }

    result.diagnostics.
        shorelineCellCount =
            shorelineCount;

    const auto finalMaterial =
        result.material.
            QueryMass(
                geology);

    const f64 newlyExcavatedBedrock =
        std::max(
            finalMaterial.
                excavatedBedrockKg -
                initialMaterial.
                    excavatedBedrockKg,
            0.0);

    const f64 finalMobile =
        result.
            sedimentExchange.
            TotalMobileMass().
            TotalKg();

    const f64 exported =
        std::max(
            result.
                sedimentExchange.
                Accounting().
                exported.
                TotalKg() -
                initialExported,
            0.0);

    const f64 materialError =
        initialMaterial.
            LooseMassKg() +
        initialMobile +
        newlyExcavatedBedrock -
        finalMaterial.
            LooseMassKg() -
        finalMobile -
        exported;

    const f64 materialReference =
        std::max(
            initialMaterial.
                LooseMassKg() +
                initialMobile +
                newlyExcavatedBedrock,
            1.0);

    result.diagnostics.
        finalMaterialLooseKg =
            finalMaterial.
                LooseMassKg();

    result.diagnostics.
        newlyExcavatedBedrockKg =
            newlyExcavatedBedrock;

    result.diagnostics.
        finalMobileSedimentKg =
            finalMobile;

    result.diagnostics.
        exportedSedimentKg =
            exported;

    result.diagnostics.
        materialBalanceErrorKg =
            materialError;

    result.diagnostics.
        materialBalanceRelativeError =
            std::abs(
                materialError) /
            materialReference;

    return result;
}
} // namespace orbit::terrain_water
