#include <orbit/terrain_erosion/ThermalErosion.hpp>

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
constexpr f64 kPi =
    3.14159265358979323846264338327950288;

enum class TransferKind : u8
{
    None,
    Regolith,
    Soil,
    Sand,
    Debris,
    BedrockToDebris
};

struct TransferProposal
{
    i32 targetX{-1};
    i32 targetY{-1};

    TransferKind kind{TransferKind::None};

    f64 sourceDepthMeters{0.0};
    f64 depositDepthMeters{0.0};
    f64 massKg{0.0};
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
    const i32 coordinate,
    const u32 resolution) noexcept
{
    return
        coordinate >= 0 &&
        coordinate <
            static_cast<i32>(resolution);
}

[[nodiscard]] f64 DegreesToRadians(
    const f64 degrees) noexcept
{
    return
        degrees *
        kPi /
        180.0;
}

[[nodiscard]] f64 RadiansToDegrees(
    const f64 radians) noexcept
{
    return
        radians *
        180.0 /
        kPi;
}

[[nodiscard]] f64 SurfaceHeight(
    const terrain_material_column::MaterialColumnPage& page,
    const u32 x,
    const u32 y) noexcept
{
    return static_cast<f64>(
        page.At(x, y).
            SurfaceHeightMeters());
}

[[nodiscard]] f64 ProtectionAllowance(
    const std::span<const f32> protection,
    const std::size_t index) noexcept
{
    if (protection.empty())
    {
        return 1.0;
    }

    return
        1.0 -
        std::clamp(
            static_cast<f64>(
                protection[index]),
            0.0,
            1.0);
}

[[nodiscard]] f64 ReposeDegrees(
    const terrain_material_column::ExposedSurfaceKind surface,
    const ThermalErosionConfig& config) noexcept
{
    switch (surface)
    {
    case terrain_material_column::ExposedSurfaceKind::Regolith:
        return config.regolithReposeDegrees;
    case terrain_material_column::ExposedSurfaceKind::Soil:
        return config.soilReposeDegrees;
    case terrain_material_column::ExposedSurfaceKind::Sand:
        return config.sandReposeDegrees;
    case terrain_material_column::ExposedSurfaceKind::Debris:
        return config.debrisReposeDegrees;
    case terrain_material_column::ExposedSurfaceKind::Bedrock:
        return config.minimumBedrockFailureDegrees;
    }

    return config.debrisReposeDegrees;
}

[[nodiscard]] f64 LooseAvailableDepth(
    const terrain_material_column::MaterialColumnCell& cell,
    const terrain_material_column::ExposedSurfaceKind surface) noexcept
{
    switch (surface)
    {
    case terrain_material_column::ExposedSurfaceKind::Regolith:
        return cell.regolithMeters;
    case terrain_material_column::ExposedSurfaceKind::Soil:
        return cell.soilMeters;
    case terrain_material_column::ExposedSurfaceKind::Sand:
        return cell.sandMeters;
    case terrain_material_column::ExposedSurfaceKind::Debris:
        return cell.debrisMeters;
    case terrain_material_column::ExposedSurfaceKind::Bedrock:
        return 0.0;
    }

    return 0.0;
}

[[nodiscard]] TransferKind ToTransferKind(
    const terrain_material_column::ExposedSurfaceKind surface) noexcept
{
    switch (surface)
    {
    case terrain_material_column::ExposedSurfaceKind::Regolith:
        return TransferKind::Regolith;
    case terrain_material_column::ExposedSurfaceKind::Soil:
        return TransferKind::Soil;
    case terrain_material_column::ExposedSurfaceKind::Sand:
        return TransferKind::Sand;
    case terrain_material_column::ExposedSurfaceKind::Debris:
        return TransferKind::Debris;
    case terrain_material_column::ExposedSurfaceKind::Bedrock:
        return TransferKind::BedrockToDebris;
    }

    return TransferKind::None;
}

[[nodiscard]] f64 TransferDensity(
    const terrain_material_column::MaterialColumnPage& page,
    const TransferKind kind) noexcept
{
    switch (kind)
    {
    case TransferKind::Regolith:
        return page.Densities().
            regolithKgPerCubicMeter;
    case TransferKind::Soil:
        return page.Densities().
            soilKgPerCubicMeter;
    case TransferKind::Sand:
        return page.Densities().
            sandKgPerCubicMeter;
    case TransferKind::Debris:
    case TransferKind::BedrockToDebris:
        return page.Densities().
            debrisKgPerCubicMeter;
    case TransferKind::None:
        return 1.0;
    }

    return 1.0;
}

[[nodiscard]] f64 BedrockFailureAngleDegrees(
    const terrain_geology::GeologicalMaterial& rock,
    const ThermalErosionConfig& config) noexcept
{
    const f64 competence =
        0.55 *
            std::clamp(
                static_cast<f64>(
                    rock.hardness),
                0.0,
                1.0) +
        0.45 *
            std::clamp(
                static_cast<f64>(
                    rock.cohesion),
                0.0,
                1.0);

    return
        config.
            minimumBedrockFailureDegrees +
        config.
            bedrockFailureAngleRangeDegrees *
            competence;
}

[[nodiscard]] f64 BedrockFractureMobility(
    const terrain_geology::GeologicalMaterial& rock,
    const ThermalErosionConfig& config) noexcept
{
    const f64 fracture =
        std::clamp(
            static_cast<f64>(
                rock.fractureTendency),
            0.0,
            1.0);

    const f64 hardness =
        std::clamp(
            static_cast<f64>(
                rock.hardness),
            0.0,
            1.0);

    const f64 cohesion =
        std::clamp(
            static_cast<f64>(
                rock.cohesion),
            0.0,
            1.0);

    return
        config.bedrockFractureRate *
        fracture *
        (1.0 - 0.70 * hardness) *
        (1.0 - 0.50 * cohesion);
}

struct SteepestNeighbor
{
    i32 x{-1};
    i32 y{-1};
    f64 dropMeters{0.0};
    f64 distanceMeters{1.0};
    f64 angleDegrees{0.0};
};

[[nodiscard]] SteepestNeighbor FindSteepestLowerNeighbor(
    const terrain_material_column::MaterialColumnPage& page,
    const u32 x,
    const u32 y) noexcept
{
    const u32 resolution =
        page.Resolution();

    const f64 source =
        SurfaceHeight(
            page,
            x,
            y);

    SteepestNeighbor best{};

    constexpr std::array<std::pair<i32, i32>, 8>
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

        const f64 target =
            SurfaceHeight(
                page,
                static_cast<u32>(nx),
                static_cast<u32>(ny));

        const f64 drop =
            source -
            target;

        if (drop <= 0.0)
        {
            continue;
        }

        const bool diagonal =
            dx != 0 &&
            dy != 0;

        const f64 distance =
            page.SpacingMeters() *
            (diagonal
                 ? 1.4142135623730951
                 : 1.0);

        const f64 angle =
            RadiansToDegrees(
                std::atan2(
                    drop,
                    distance));

        if (angle >
            best.angleDegrees)
        {
            best = {
                .x = nx,
                .y = ny,
                .dropMeters = drop,
                .distanceMeters = distance,
                .angleDegrees = angle
            };
        }
    }

    return best;
}
} // namespace

bool ThermalErosionConfig::IsValid() const noexcept
{
    const auto angle =
        [](const f64 value)
        {
            return
                std::isfinite(value) &&
                value > 0.0 &&
                value < 90.0;
        };

    return
        maximumIterations > 0U &&
        angle(sandReposeDegrees) &&
        angle(debrisReposeDegrees) &&
        angle(regolithReposeDegrees) &&
        angle(soilReposeDegrees) &&
        angle(minimumBedrockFailureDegrees) &&
        std::isfinite(
            bedrockFailureAngleRangeDegrees) &&
        bedrockFailureAngleRangeDegrees >= 0.0 &&
        minimumBedrockFailureDegrees +
                bedrockFailureAngleRangeDegrees <
            90.0 &&
        std::isfinite(
            bedrockFractureRate) &&
        bedrockFractureRate >= 0.0 &&
        bedrockFractureRate <= 1.0 &&
        std::isfinite(relaxation) &&
        relaxation > 0.0 &&
        relaxation <= 1.0 &&
        std::isfinite(
            maximumTransferDepthPerIterationMeters) &&
        maximumTransferDepthPerIterationMeters >=
            0.0 &&
        std::isfinite(
            convergenceDepthMeters) &&
        convergenceDepthMeters >= 0.0;
}

const ThermalCellState&
ThermalErosionResult::At(
    const u32 x,
    const u32 y) const
{
    if (x >= material.Resolution() ||
        y >= material.Resolution())
    {
        throw std::out_of_range(
            "Orbit M12 thermal result coordinate is out of range.");
    }

    return cells[Index(
        material.Resolution(),
        x,
        y)];
}

ThermalErosionResult SimulateThermalErosion(
    terrain_material_column::MaterialColumnPage material,
    const terrain_geology::GeologicalMaterialLibrary& geology,
    const std::span<const f32> protection,
    const ThermalErosionConfig& config)
{
    if (!config.IsValid())
    {
        throw std::invalid_argument(
            "Orbit M12 thermal erosion configuration is invalid.");
    }

    const u32 resolution =
        material.Resolution();

    const std::size_t cellCount =
        static_cast<std::size_t>(
            resolution) *
        resolution;

    if (!protection.empty() &&
        protection.size() !=
            cellCount)
    {
        throw std::invalid_argument(
            "Orbit M12 protection field must match the physical page.");
    }

    for (const f32 value : protection)
    {
        if (!std::isfinite(value) ||
            value < 0.0F ||
            value > 1.0F)
        {
            throw std::invalid_argument(
                "Orbit M12 protection values must be finite [0,1].");
        }
    }

    const auto initialMass =
        material.QueryMass(
            geology);

    ThermalErosionResult result{
        .material = std::move(material),
        .cells =
            std::vector<ThermalCellState>(
                cellCount)
    };

    std::vector<TransferProposal>
        proposals(
            cellCount);

    std::vector<f64> deltaRegolith(
        cellCount,
        0.0);

    std::vector<f64> deltaSoil(
        cellCount,
        0.0);

    std::vector<f64> deltaSand(
        cellCount,
        0.0);

    std::vector<f64> deltaDebris(
        cellCount,
        0.0);

    std::vector<f64> deltaBedrock(
        cellCount,
        0.0);

    const f64 area =
        result.material.
            CellAreaSquareMeters();

    for (u32 iteration = 0U;
         iteration <
             config.maximumIterations;
         ++iteration)
    {
        std::fill(
            proposals.begin(),
            proposals.end(),
            TransferProposal{});

        f64 maximumMoveDepth = 0.0;

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
                    result.material.At(
                        x,
                        y);

                const auto exposed =
                    cell.ExposedSurface();

                const SteepestNeighbor neighbor =
                    FindSteepestLowerNeighbor(
                        result.material,
                        x,
                        y);

                ThermalCellState& state =
                    result.cells[index];

                state.maximumSlopeDegrees =
                    static_cast<f32>(
                        neighbor.
                            angleDegrees);

                const f64 allowance =
                    ProtectionAllowance(
                        protection,
                        index);

                if (allowance <= 0.0 ||
                    neighbor.x < 0 ||
                    neighbor.y < 0)
                {
                    state.unstable = false;
                    continue;
                }

                f64 criticalAngle =
                    ReposeDegrees(
                        exposed,
                        config);

                f64 mobility = 1.0;
                f64 sourceDensity = 0.0;
                f64 depositDensity = 0.0;
                f64 availableDepth = 0.0;

                TransferKind kind =
                    ToTransferKind(
                        exposed);

                if (exposed ==
                    terrain_material_column::
                        ExposedSurfaceKind::
                            Bedrock)
                {
                    const auto* rock =
                        geology.Find(
                            cell.
                                bedrockMaterial);

                    if (rock == nullptr)
                    {
                        throw std::invalid_argument(
                            "Orbit M12 encountered unknown M02 bedrock.");
                    }

                    criticalAngle =
                        BedrockFailureAngleDegrees(
                            *rock,
                            config);

                    mobility =
                        BedrockFractureMobility(
                            *rock,
                            config);

                    sourceDensity =
                        rock->density;

                    depositDensity =
                        result.material.
                            Densities().
                            debrisKgPerCubicMeter;

                    availableDepth =
                        config.
                            maximumTransferDepthPerIterationMeters;
                }
                else
                {
                    availableDepth =
                        LooseAvailableDepth(
                            cell,
                            exposed);

                    sourceDensity =
                        TransferDensity(
                            result.material,
                            kind);

                    depositDensity =
                        sourceDensity;
                }

                state.activeReposeDegrees =
                    static_cast<f32>(
                        criticalAngle);

                state.unstable =
                    neighbor.angleDegrees >
                    criticalAngle;

                if (!state.unstable ||
                    availableDepth <= 0.0 ||
                    mobility <= 0.0)
                {
                    continue;
                }

                const f64 allowedDrop =
                    std::tan(
                        DegreesToRadians(
                            criticalAngle)) *
                    neighbor.distanceMeters;

                const f64 excessHeight =
                    std::max(
                        neighbor.dropMeters -
                            allowedDrop,
                        0.0);

                // Moving equal-density loose material from source to receiver
                // changes their height difference by roughly 2*d. Bedrock
                // fracture uses the same geometric target but converts depth by
                // mass into the debris density at the receiver.
                const f64 sourceDepth =
                    std::min({
                        0.5 *
                            excessHeight *
                            config.relaxation *
                            allowance *
                            mobility,
                        availableDepth,
                        config.
                            maximumTransferDepthPerIterationMeters
                    });

                if (sourceDepth <= 0.0)
                {
                    continue;
                }

                const f64 mass =
                    sourceDepth *
                    area *
                    sourceDensity;

                const f64 depositDepth =
                    mass /
                    std::max(
                        area *
                            depositDensity,
                        1.0e-12);

                proposals[index] = {
                    .targetX =
                        neighbor.x,
                    .targetY =
                        neighbor.y,
                    .kind = kind,
                    .sourceDepthMeters =
                        sourceDepth,
                    .depositDepthMeters =
                        depositDepth,
                    .massKg = mass
                };

                maximumMoveDepth =
                    std::max(
                        maximumMoveDepth,
                        sourceDepth);
            }
        }

        result.iterationsExecuted =
            iteration + 1U;

        if (maximumMoveDepth <=
            config.convergenceDepthMeters)
        {
            result.converged = true;
            break;
        }

        std::fill(
            deltaRegolith.begin(),
            deltaRegolith.end(),
            0.0);

        std::fill(
            deltaSoil.begin(),
            deltaSoil.end(),
            0.0);

        std::fill(
            deltaSand.begin(),
            deltaSand.end(),
            0.0);

        std::fill(
            deltaDebris.begin(),
            deltaDebris.end(),
            0.0);

        std::fill(
            deltaBedrock.begin(),
            deltaBedrock.end(),
            0.0);

        for (u32 y = 0U;
             y < resolution;
             ++y)
        {
            for (u32 x = 0U;
                 x < resolution;
                 ++x)
            {
                const std::size_t sourceIndex =
                    Index(
                        resolution,
                        x,
                        y);

                const TransferProposal& proposal =
                    proposals[sourceIndex];

                if (proposal.kind ==
                        TransferKind::None ||
                    proposal.sourceDepthMeters <=
                        0.0)
                {
                    continue;
                }

                const std::size_t targetIndex =
                    Index(
                        resolution,
                        static_cast<u32>(
                            proposal.targetX),
                        static_cast<u32>(
                            proposal.targetY));

                switch (proposal.kind)
                {
                case TransferKind::Regolith:
                    deltaRegolith[sourceIndex] -=
                        proposal.
                            sourceDepthMeters;

                    deltaRegolith[targetIndex] +=
                        proposal.
                            depositDepthMeters;
                    break;

                case TransferKind::Soil:
                    deltaSoil[sourceIndex] -=
                        proposal.
                            sourceDepthMeters;

                    deltaSoil[targetIndex] +=
                        proposal.
                            depositDepthMeters;
                    break;

                case TransferKind::Sand:
                    deltaSand[sourceIndex] -=
                        proposal.
                            sourceDepthMeters;

                    deltaSand[targetIndex] +=
                        proposal.
                            depositDepthMeters;
                    break;

                case TransferKind::Debris:
                    deltaDebris[sourceIndex] -=
                        proposal.
                            sourceDepthMeters;

                    deltaDebris[targetIndex] +=
                        proposal.
                            depositDepthMeters;
                    break;

                case TransferKind::BedrockToDebris:
                    deltaBedrock[sourceIndex] -=
                        proposal.
                            sourceDepthMeters;

                    deltaDebris[targetIndex] +=
                        proposal.
                            depositDepthMeters;

                    result.cells[sourceIndex].
                        producedDebrisKg +=
                            proposal.massKg;
                    break;

                case TransferKind::None:
                    break;
                }

                result.cells[sourceIndex].
                    movedOutKg +=
                        proposal.massKg;

                result.cells[targetIndex].
                    receivedKg +=
                        proposal.massKg;
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

                auto cell =
                    result.material.At(
                        x,
                        y);

                const auto updateLoose =
                    [](const f32 current,
                       const f64 delta)
                    {
                        return static_cast<f32>(
                            std::max(
                                static_cast<f64>(
                                    current) +
                                    delta,
                                0.0));
                    };

                cell.regolithMeters =
                    updateLoose(
                        cell.regolithMeters,
                        deltaRegolith[index]);

                cell.soilMeters =
                    updateLoose(
                        cell.soilMeters,
                        deltaSoil[index]);

                cell.sandMeters =
                    updateLoose(
                        cell.sandMeters,
                        deltaSand[index]);

                cell.debrisMeters =
                    updateLoose(
                        cell.debrisMeters,
                        deltaDebris[index]);

                cell.bedrockHeightMeters =
                    static_cast<f32>(
                        static_cast<f64>(
                            cell.
                                bedrockHeightMeters) +
                        deltaBedrock[index]);

                result.material.SetCell(
                    x,
                    y,
                    cell,
                    false);
            }
        }
    }

    const auto finalMass =
        result.material.QueryMass(
            geology);

    const f64 fracturedBedrock =
        std::max(
            finalMass.
                excavatedBedrockKg -
                initialMass.
                    excavatedBedrockKg,
            0.0);

    const f64 error =
        initialMass.LooseMassKg() +
        fracturedBedrock -
        finalMass.LooseMassKg();

    const f64 reference =
        std::max(
            initialMass.LooseMassKg() +
                fracturedBedrock,
            1.0);

    result.massBalance = {
        .initialLooseMassKg =
            initialMass.
                LooseMassKg(),
        .finalLooseMassKg =
            finalMass.
                LooseMassKg(),
        .fracturedBedrockMassKg =
            fracturedBedrock,
        .materialBalanceErrorKg =
            error,
        .materialBalanceRelativeError =
            std::abs(error) /
            reference
    };

    return result;
}
} // namespace orbit::terrain_erosion
