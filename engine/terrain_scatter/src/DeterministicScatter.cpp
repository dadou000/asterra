#include <orbit/terrain_scatter/DeterministicScatter.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace orbit::terrain_scatter
{
namespace
{
[[nodiscard]] u32 Hash32(
    u32 value) noexcept
{
    value ^= value >> 16U;
    value *= 0x7FEB352DU;
    value ^= value >> 15U;
    value *= 0x846CA68BU;
    value ^= value >> 16U;
    return value;
}

[[nodiscard]] u32 Fold64(
    const u64 value) noexcept
{
    return
        static_cast<u32>(value) ^
        static_cast<u32>(value >> 32U);
}

[[nodiscard]] u32 RuleHashInternal(
    const terrain_biome::BiomeScatterLayerRule& rule) noexcept
{
    return Hash32(
        Fold64(rule.id.high) ^
        Hash32(Fold64(rule.id.low)) ^
        rule.seedSalt);
}

[[nodiscard]] u32 CandidateBaseHash(
    const u32 pageHash,
    const terrain_biome::BiomeScatterLayerRule& rule,
    const u32 x,
    const u32 y) noexcept
{
    return Hash32(
        pageHash ^
        RuleHashInternal(rule) ^
        Hash32(x * 0x9E3779B9U) ^
        Hash32(y * 0x85EBCA6BU));
}

[[nodiscard]] f32 UnitRandom(
    const u32 value) noexcept
{
    return
        static_cast<f32>(
            value >> 8U) *
        (1.0F /
         16'777'216.0F);
}

[[nodiscard]] f32 SmoothStep01(
    const f32 value) noexcept
{
    const f32 t =
        std::clamp(
            value,
            0.0F,
            1.0F);

    return
        t * t *
        (3.0F -
         2.0F * t);
}

[[nodiscard]] f32 RangeMask(
    const f32 value,
    const f32 minimum,
    const f32 maximum,
    const f32 falloff) noexcept
{
    if (value >= minimum &&
        value <= maximum)
    {
        return 1.0F;
    }

    if (falloff <= 0.0F)
    {
        return 0.0F;
    }

    if (value < minimum)
    {
        if (value <=
            minimum -
                falloff)
        {
            return 0.0F;
        }

        return
            SmoothStep01(
                (value -
                 (minimum -
                  falloff)) /
                falloff);
    }

    if (value >=
        maximum +
            falloff)
    {
        return 0.0F;
    }

    return
        1.0F -
        SmoothStep01(
            (value -
             maximum) /
            falloff);
}

[[nodiscard]] terrain_biome::BiomeExposedMaterialMask
ExposedMask(
    const terrain_material_column::ExposedSurfaceKind kind) noexcept
{
    using Surface =
        terrain_material_column::
            ExposedSurfaceKind;

    using Mask =
        terrain_biome::
            BiomeExposedMaterialMask;

    switch (kind)
    {
    case Surface::Bedrock:
        return Mask::Bedrock;
    case Surface::Regolith:
        return Mask::Regolith;
    case Surface::Soil:
        return Mask::Soil;
    case Surface::Sand:
        return Mask::Sand;
    case Surface::Debris:
        return Mask::Debris;
    }

    return Mask::None;
}

[[nodiscard]] bool Compatible(
    const terrain_biome::BiomeScatterLayerRule& rule,
    const ScatterCellInput& cell) noexcept
{
    const u32 compatibility =
        static_cast<u32>(
            rule.compatibleExposed);

    const u32 exposed =
        static_cast<u32>(
            ExposedMask(
                cell.exposedMaterial));

    if ((compatibility &
         exposed) ==
        0U)
    {
        return false;
    }

    // Soil-dependent vegetation must be on actual exposed soil as well as
    // having sufficient soil depth. This guarantees bare rock cannot grow a
    // tree merely because a stale depth channel is non-zero.
    if (rule.requiresSoil &&
        (cell.exposedMaterial !=
             terrain_material_column::
                 ExposedSurfaceKind::Soil ||
         cell.soilDepthMeters <
             rule.minimumSoilDepthMeters))
    {
        return false;
    }

    return true;
}

[[nodiscard]] f32 CandidateProbability(
    const ScatterPageRequest& request,
    const ScatterCellInput& cell) noexcept
{
    if (!Compatible(
            request.rule,
            cell))
    {
        return 0.0F;
    }

    const f32 slope =
        RangeMask(
            cell.slopeDegrees,
            request.rule.
                minimumSlopeDegrees,
            request.rule.
                maximumSlopeDegrees,
            request.rule.
                slopeFalloffDegrees);

    const f32 moisture =
        RangeMask(
            cell.moisture,
            request.rule.
                minimumMoisture,
            request.rule.
                maximumMoisture,
            request.rule.
                moistureFalloff);

    const f32 exclusion =
        1.0F -
        cell.exclusionMask;

    const f32 cellArea =
        request.cellSizeMeters *
        request.cellSizeMeters;

    return
        std::clamp(
            request.rule.
                densityPerSquareMeter *
                cellArea *
                request.
                    biomeDensityMultiplier *
                cell.biomeWeight *
                cell.authoredDensity *
                slope *
                moisture *
                exclusion,
            0.0F,
            1.0F);
}

struct Candidate
{
    math::Double2 position{};
    u32 baseHash{0};
    u32 priority{0};
    f32 probability{0.0F};
    bool densityAccepted{false};
};

[[nodiscard]] Candidate BuildCandidate(
    const ScatterPageRequest& request,
    const ScatterCellInput& cell,
    const u32 x,
    const u32 y) noexcept
{
    const u32 base =
        CandidateBaseHash(
            ScatterPageHash(
                request.identity),
            request.rule,
            x,
            y);

    const f32 jitterX =
        UnitRandom(
            Hash32(
                base ^
                0xA341316CU));

    const f32 jitterY =
        UnitRandom(
            Hash32(
                base ^
                0xC8013EA4U));

    const f64 half =
        static_cast<f64>(
            request.gridResolution) *
        0.5;

    const math::Double2 position{
        (static_cast<f64>(x) +
         static_cast<f64>(jitterX) -
         half) *
            request.cellSizeMeters,
        (static_cast<f64>(y) +
         static_cast<f64>(jitterY) -
         half) *
            request.cellSizeMeters
    };

    const u32 priority =
        Hash32(
            base ^
            0xAD90777DU);

    const f32 probability =
        CandidateProbability(
            request,
            cell);

    const f32 densityDraw =
        UnitRandom(
            Hash32(
                base ^
                0x7E95761EU));

    return {
        .position = position,
        .baseHash = base,
        .priority = priority,
        .probability = probability,
        .densityAccepted =
            densityDraw <
            probability
    };
}

[[nodiscard]] bool HasLowerPriorityConflict(
    const ScatterPageRequest& request,
    const std::span<const ScatterCellInput> cells,
    const Candidate& self,
    const u32 selfX,
    const u32 selfY)
{
    const i32 resolution =
        static_cast<i32>(
            request.gridResolution);

    const f64 minimumSpacingSquared =
        static_cast<f64>(
            request.rule.
                minimumSpacingMeters) *
        request.rule.
            minimumSpacingMeters;

    for (i32 dy = -1;
         dy <= 1;
         ++dy)
    {
        for (i32 dx = -1;
             dx <= 1;
             ++dx)
        {
            if (dx == 0 &&
                dy == 0)
            {
                continue;
            }

            const i32 nx =
                static_cast<i32>(
                    selfX) +
                dx;

            const i32 ny =
                static_cast<i32>(
                    selfY) +
                dy;

            if (nx < 0 ||
                ny < 0 ||
                nx >= resolution ||
                ny >= resolution)
            {
                continue;
            }

            const u32 ux =
                static_cast<u32>(nx);

            const u32 uy =
                static_cast<u32>(ny);

            const std::size_t index =
                static_cast<std::size_t>(
                    uy) *
                    request.gridResolution +
                ux;

            const Candidate neighbor =
                BuildCandidate(
                    request,
                    cells[index],
                    ux,
                    uy);

            if (!neighbor.
                    densityAccepted)
            {
                continue;
            }

            const math::Double2 delta =
                neighbor.position -
                self.position;

            const f64 distanceSquared =
                delta.x * delta.x +
                delta.y * delta.y;

            if (distanceSquared >=
                minimumSpacingSquared)
            {
                continue;
            }

            if (neighbor.priority <
                    self.priority ||
                (neighbor.priority ==
                     self.priority &&
                 (uy <
                      selfY ||
                  (uy ==
                       selfY &&
                   ux <
                       selfX))))
            {
                return true;
            }
        }
    }

    return false;
}

[[nodiscard]] DerivedScatterInstanceId
InstanceId(
    const Candidate& candidate) noexcept
{
    DerivedScatterInstanceId id{
        .high =
            static_cast<u64>(
                candidate.baseHash) <<
                32U |
            Hash32(
                candidate.baseHash ^
                0xD3A2646CU),
        .low =
            static_cast<u64>(
                Hash32(
                    candidate.baseHash ^
                    0xFD7046C5U)) <<
                32U |
            Hash32(
                candidate.baseHash ^
                0xB55A4F09U)
    };

    if (!id.IsValid())
    {
        id.low = 1U;
    }

    return id;
}
} // namespace

bool ScatterPageIdentity::IsValid() const noexcept
{
    return planet.IsValid();
}

bool ScatterCellInput::IsValid() const noexcept
{
    const auto unit =
        [](const f32 value)
        {
            return
                std::isfinite(value) &&
                value >= 0.0F &&
                value <= 1.0F;
        };

    return
        unit(biomeWeight) &&
        std::isfinite(
            slopeDegrees) &&
        slopeDegrees >= 0.0F &&
        slopeDegrees <= 90.0F &&
        std::isfinite(
            soilDepthMeters) &&
        soilDepthMeters >= 0.0F &&
        unit(moisture) &&
        unit(exclusionMask) &&
        std::isfinite(
            authoredDensity) &&
        authoredDensity >= 0.0F;
}

bool ScatterPageRequest::IsValid() const noexcept
{
    return
        identity.IsValid() &&
        gridResolution > 0U &&
        std::isfinite(
            cellSizeMeters) &&
        cellSizeMeters >=
            rule.minimumSpacingMeters &&
        rule.IsValid() &&
        std::isfinite(
            biomeDensityMultiplier) &&
        biomeDensityMultiplier >= 0.0F;
}

bool DerivedScatterInstance::IsValid() const noexcept
{
    return
        id.IsValid() &&
        std::isfinite(
            localOffsetMeters.x) &&
        std::isfinite(
            localOffsetMeters.y) &&
        std::isfinite(
            yawRadians) &&
        yawRadians >= 0.0F &&
        yawRadians <
            static_cast<f32>(
                2.0 *
                std::numbers::pi_v<f64>) &&
        std::isfinite(
            uniformScale) &&
        uniformScale > 0.0F;
}

u32 ScatterPageHash(
    const ScatterPageIdentity& identity) noexcept
{
    u32 hash =
        Hash32(
            Fold64(
                identity.planet.high) ^
            0x4D323250U);

    hash =
        Hash32(
            hash ^
            Fold64(
                identity.planet.low));

    hash =
        Hash32(
            hash ^
            static_cast<u32>(
                identity.tile.face));

    hash =
        Hash32(
            hash ^
            static_cast<u32>(
                identity.tile.level) *
                0x9E3779B9U);

    hash =
        Hash32(
            hash ^
            Hash32(
                identity.tile.x));

    hash =
        Hash32(
            hash ^
            Hash32(
                identity.tile.y));

    hash =
        Hash32(
            hash ^
            Fold64(
                identity.
                    sourceRevision));

    hash =
        Hash32(
            hash ^
            Fold64(
                identity.
                    scatterRevision));

    return
        Hash32(
            hash ^
            Fold64(
                identity.
                    generationSeed));
}

u32 ScatterRuleHash(
    const terrain_biome::BiomeScatterLayerRule& rule) noexcept
{
    return
        RuleHashInternal(
            rule);
}

std::vector<DerivedScatterInstance>
GenerateDeterministicScatter(
    const ScatterPageRequest& request,
    const std::span<const ScatterCellInput> cells)
{
    if (!request.IsValid())
    {
        throw std::invalid_argument(
            "Orbit M22 scatter page request is invalid.");
    }

    const std::size_t required =
        static_cast<std::size_t>(
            request.gridResolution) *
        request.gridResolution;

    if (cells.size() !=
        required)
    {
        throw std::invalid_argument(
            "Orbit M22 scatter field size does not match the planting grid.");
    }

    for (const auto& cell :
         cells)
    {
        if (!cell.IsValid())
        {
            throw std::invalid_argument(
                "Orbit M22 scatter field contains invalid physical input.");
        }
    }

    std::vector<DerivedScatterInstance>
        instances;

    instances.reserve(
        required /
        2U);

    for (u32 y = 0U;
         y <
             request.gridResolution;
         ++y)
    {
        for (u32 x = 0U;
             x <
                 request.gridResolution;
             ++x)
        {
            const std::size_t index =
                static_cast<std::size_t>(
                    y) *
                    request.gridResolution +
                x;

            const Candidate candidate =
                BuildCandidate(
                    request,
                    cells[index],
                    x,
                    y);

            if (!candidate.
                    densityAccepted ||
                HasLowerPriorityConflict(
                    request,
                    cells,
                    candidate,
                    x,
                    y))
            {
                continue;
            }

            const f32 yaw =
                UnitRandom(
                    Hash32(
                        candidate.baseHash ^
                        0xB8E1AFEDU)) *
                static_cast<f32>(
                    2.0 *
                    std::numbers::pi_v<f64>);

            const f32 scaleT =
                UnitRandom(
                    Hash32(
                        candidate.baseHash ^
                        0x6C8E9CF5U));

            const f32 scale =
                request.rule.
                    minimumScale +
                (request.rule.
                     maximumScale -
                 request.rule.
                     minimumScale) *
                    scaleT;

            DerivedScatterInstance instance{
                .id =
                    InstanceId(
                        candidate),
                .kind =
                    request.rule.kind,
                .localOffsetMeters =
                    candidate.position,
                .yawRadians =
                    yaw,
                .uniformScale =
                    scale,
                .cellX = x,
                .cellY = y
            };

            if (!instance.IsValid())
            {
                throw std::logic_error(
                    "Orbit M22 deterministic scatter produced an invalid instance.");
            }

            instances.push_back(
                instance);
        }
    }

    return instances;
}
} // namespace orbit::terrain_scatter
