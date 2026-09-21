#include <orbit/lighting/RadianceClipmapResidency.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace orbit::lighting
{
namespace
{
[[nodiscard]] u32 PositiveModulo(
    const i64 value,
    const u32 modulus) noexcept
{
    const i64 m =
        static_cast<i64>(modulus);

    const i64 result =
        ((value % m) + m) % m;

    return static_cast<u32>(result);
}

[[nodiscard]] f64 DistanceSquared(
    const math::Double3& a,
    const math::Double3& b) noexcept
{
    const auto d = a - b;
    return math::Dot(d, d);
}
} // namespace

RadianceClipmapResidency::RadianceClipmapResidency(
    RadianceClipmapConfig config)
    : config_(config)
{
    static_cast<void>(
        BuildRadianceClipmapMemoryLayout(
            config_));

    levels_.reserve(
        config_.levelCount);

    const u64 axis =
        static_cast<u64>(
            config_.cellsPerAxis);
    const u64 count =
        axis * axis * axis;

    if (count >
        static_cast<u64>(
            std::numeric_limits<std::size_t>::max()))
    {
        throw std::overflow_error(
            "Radiance residency level exceeds addressable memory.");
    }

    for (u32 level = 0U;
         level < config_.levelCount;
         ++level)
    {
        Level state;
        state.level = level;
        state.slots.resize(
            static_cast<std::size_t>(
                count));
        levels_.push_back(
            std::move(state));
    }
}

void RadianceClipmapResidency::Reset(
    const LightingView& view,
    const math::Double3& observerInFrameMeters,
    const u64 sourceRevision)
{
    frame_ = view.frame;
    body_ = view.body;
    sourceRevision_ = sourceRevision;

    if (!frame_ || !body_)
    {
        throw std::invalid_argument(
            "Radiance residency requires a valid body/frame.");
    }

    for (auto& level : levels_)
    {
        for (auto& slot : level.slots)
        {
            slot = {};
        }
    }

    static_cast<void>(
        ScrollTo(
            view,
            observerInFrameMeters,
            sourceRevision,
            0.0F));
}

u32 RadianceClipmapResidency::PhysicalIndex(
    const RadianceCellKey& key) const noexcept
{
    const u32 axis =
        config_.cellsPerAxis;

    const u32 x =
        PositiveModulo(
            key.x,
            axis);
    const u32 y =
        PositiveModulo(
            key.y,
            axis);
    const u32 z =
        PositiveModulo(
            key.z,
            axis);

    return
        x +
        axis *
            (y + axis * z);
}

RadianceResidentCell*
RadianceClipmapResidency::SlotFor(
    const RadianceCellKey& key) noexcept
{
    if (key.level >= levels_.size())
    {
        return nullptr;
    }

    auto& level =
        levels_[key.level];

    return
        &level.slots[
            PhysicalIndex(key)];
}

const RadianceResidentCell*
RadianceClipmapResidency::SlotFor(
    const RadianceCellKey& key) const noexcept
{
    if (key.level >= levels_.size())
    {
        return nullptr;
    }

    const auto& level =
        levels_[key.level];

    return
        &level.slots[
            PhysicalIndex(key)];
}

bool RadianceClipmapResidency::BelongsToCurrentWindow(
    const RadianceCellKey& key) const noexcept
{
    if (key.frame != frame_ ||
        key.body != body_ ||
        key.level >= levels_.size())
    {
        return false;
    }

    const auto& level =
        levels_[key.level];

    const i64 half =
        static_cast<i64>(
            config_.cellsPerAxis / 2U);

    const i64 minimumX =
        level.centerX - half;
    const i64 minimumY =
        level.centerY - half;
    const i64 minimumZ =
        level.centerZ - half;

    const i64 maximumX =
        minimumX +
        static_cast<i64>(
            config_.cellsPerAxis) - 1;
    const i64 maximumY =
        minimumY +
        static_cast<i64>(
            config_.cellsPerAxis) - 1;
    const i64 maximumZ =
        minimumZ +
        static_cast<i64>(
            config_.cellsPerAxis) - 1;

    return
        key.x >= minimumX &&
        key.x <= maximumX &&
        key.y >= minimumY &&
        key.y <= maximumY &&
        key.z >= minimumZ &&
        key.z <= maximumZ;
}

RadianceResidencyStats
RadianceClipmapResidency::ScrollTo(
    const LightingView& view,
    const math::Double3& observerInFrameMeters,
    const u64 sourceRevision,
    const f32 deltaSeconds)
{
    if (!view.frame || !view.body)
    {
        throw std::invalid_argument(
            "Radiance residency requires a valid body/frame.");
    }

    const bool authorityChanged =
        frame_ != view.frame ||
        body_ != view.body;

    if (authorityChanged)
    {
        frame_ = view.frame;
        body_ = view.body;

        for (auto& level : levels_)
        {
            for (auto& slot : level.slots)
            {
                slot = {};
            }
        }
    }

    const bool revisionChanged =
        sourceRevision_ !=
        sourceRevision;

    sourceRevision_ =
        sourceRevision;

    RadianceResidencyStats stats;

    const f32 ageDelta =
        std::isfinite(deltaSeconds)
            ? std::max(
                  deltaSeconds,
                  0.0F)
            : 0.0F;

    for (auto& level : levels_)
    {
        const auto centerKey =
            RadianceCellForPoint(
                observerInFrameMeters,
                config_,
                level.level,
                view);

        level.centerX = centerKey.x;
        level.centerY = centerKey.y;
        level.centerZ = centerKey.z;

        const i64 half =
            static_cast<i64>(
                config_.cellsPerAxis / 2U);

        const i64 firstX =
            level.centerX - half;
        const i64 firstY =
            level.centerY - half;
        const i64 firstZ =
            level.centerZ - half;

        for (u32 z = 0U;
             z < config_.cellsPerAxis;
             ++z)
        {
            for (u32 y = 0U;
                 y < config_.cellsPerAxis;
                 ++y)
            {
                for (u32 x = 0U;
                     x < config_.cellsPerAxis;
                     ++x)
                {
                    const RadianceCellKey desired{
                        .frame = frame_,
                        .body = body_,
                        .level = level.level,
                        .x = firstX +
                            static_cast<i64>(x),
                        .y = firstY +
                            static_cast<i64>(y),
                        .z = firstZ +
                            static_cast<i64>(z)
                    };

                    auto* slot =
                        SlotFor(desired);

                    if (slot == nullptr)
                    {
                        continue;
                    }

                    if (slot->occupied &&
                        slot->key == desired)
                    {
                        ++stats.reusedCells;

                        slot->cell.updateAgeSeconds =
                            std::max(
                                slot->cell.
                                    updateAgeSeconds +
                                    ageDelta,
                                0.0F);

                        if (revisionChanged &&
                            slot->sourceRevision !=
                                sourceRevision_)
                        {
                            slot->dirty = true;
                            slot->cell.valid = false;
                        }
                    }
                    else
                    {
                        ++stats.replacedCells;

                        *slot = {
                            .key = desired,
                            .cell = {
                                .revision =
                                    sourceRevision_,
                                .updateAgeSeconds =
                                    std::numeric_limits<f32>::
                                        infinity(),
                                .sampleCount = 0U,
                                .valid = false
                            },
                            .sourceRevision =
                                sourceRevision_,
                            .occupied = true,
                            .dirty = true
                        };
                    }
                }
            }
        }
    }

    for (const auto& level : levels_)
    {
        for (const auto& slot :
             level.slots)
        {
            if (!slot.occupied ||
                !BelongsToCurrentWindow(
                    slot.key))
            {
                continue;
            }

            ++stats.residentCells;

            if (slot.dirty ||
                !slot.cell.valid ||
                slot.sourceRevision !=
                    sourceRevision_)
            {
                ++stats.dirtyCells;
            }
        }
    }

    return stats;
}

void RadianceClipmapResidency::InvalidateSphere(
    const math::Double3& centerInFrameMeters,
    const f64 radiusMeters,
    const u64 sourceRevision,
    const f32 priorityBoost,
    const bool preservePreviousValue)
{
    if (!std::isfinite(radiusMeters) ||
        radiusMeters < 0.0)
    {
        throw std::invalid_argument(
            "Radiance invalidation radius must be finite and non-negative.");
    }

    const u64 previousRevision =
        sourceRevision_;

    if (sourceRevision < previousRevision)
    {
        throw std::invalid_argument(
            "Radiance invalidation revision cannot move backwards.");
    }

    sourceRevision_ =
        sourceRevision;

    // A dependency-aware localized change advances the authority revision,
    // but cells proven outside the invalidation region remain physically
    // valid. Promote those cells to the new revision rather than turning a
    // local terrain/emissive edit into a global cache flush.
    for (auto& level : levels_)
    {
        const f64 cellSize =
            RadianceCellSizeMeters(
                config_,
                level.level);

        const f64 halfDiagonal =
            cellSize *
            0.8660254037844386;

        const f64 influence =
            radiusMeters +
            halfDiagonal;
        const f64 influenceSquared =
            influence *
            influence;

        for (auto& slot :
             level.slots)
        {
            if (!slot.occupied ||
                !BelongsToCurrentWindow(
                    slot.key))
            {
                continue;
            }

            const auto center =
                RadianceCellCenterInFrame(
                    slot.key,
                    config_);

            if (DistanceSquared(
                    center,
                    centerInFrameMeters) <=
                influenceSquared)
            {
                slot.dirty = true;
                slot.invalidationPriorityBoost =
                    std::max(
                        slot.invalidationPriorityBoost,
                        std::max(
                            priorityBoost,
                            0.0F));

                if (!preservePreviousValue)
                {
                    slot.cell.valid = false;
                }

                slot.sourceRevision =
                    sourceRevision;
                slot.cell.revision =
                    sourceRevision;
            }
            else if (slot.sourceRevision ==
                         previousRevision &&
                     slot.cell.revision ==
                         previousRevision)
            {
                slot.sourceRevision =
                    sourceRevision;
                slot.cell.revision =
                    sourceRevision;
            }
        }
    }
}

void RadianceClipmapResidency::RequestGlobalRefresh() noexcept
{
    for (auto& level : levels_)
    {
        for (auto& slot : level.slots)
        {
            if (!slot.occupied ||
                !BelongsToCurrentWindow(slot.key))
            {
                continue;
            }

            // Keep cell.valid intact. Dirty means "schedule a refresh";
            // invalid means "do not sample". Dynamic lighting changes need
            // the former so large caches can converge under a fixed budget.
            slot.dirty = true;
            slot.invalidationPriorityBoost =
                std::max(
                    slot.invalidationPriorityBoost,
                    0.25F);
        }
    }
}

std::vector<RadianceUpdateCandidate>
RadianceClipmapResidency::BuildUpdateList(
    const math::Double3& observerInFrameMeters,
    const u32 maximumUpdates) const
{
    std::vector<RadianceUpdateCandidate>
        candidates;

    if (maximumUpdates == 0U)
    {
        return candidates;
    }

    for (const auto& level :
         levels_)
    {
        const f64 cellSize =
            RadianceCellSizeMeters(
                config_,
                level.level);

        for (const auto& slot :
             level.slots)
        {
            if (!slot.occupied ||
                !BelongsToCurrentWindow(
                    slot.key) ||
                (!slot.dirty &&
                 slot.cell.valid &&
                 slot.sourceRevision ==
                     sourceRevision_))
            {
                continue;
            }

            const auto center =
                RadianceCellCenterInFrame(
                    slot.key,
                    config_);

            const f64 distance =
                std::sqrt(
                    DistanceSquared(
                        center,
                        observerInFrameMeters));

            const f32 normalizedDistance =
                static_cast<f32>(
                    distance /
                    std::max(
                        cellSize,
                        1.0e-6));

            const f32 age =
                std::isfinite(
                    slot.cell.
                        updateAgeSeconds)
                    ? std::max(
                          slot.cell.
                              updateAgeSeconds,
                          0.0F)
                    : 1.0e6F;

            // Fine levels and nearby/old cells win. The exact scheduler
            // weights remain policy and can evolve without changing keys.
            const f32 levelWeight =
                1.0F /
                (1.0F +
                 static_cast<f32>(
                     level.level));

            const f32 distanceWeight =
                1.0F /
                (1.0F +
                 normalizedDistance);

            const f32 ageWeight =
                1.0F +
                std::min(
                    age,
                    60.0F) /
                    60.0F;

            candidates.push_back({
                .key = slot.key,
                .physicalIndex =
                    PhysicalIndex(
                        slot.key),
                .priority =
                    levelWeight *
                    distanceWeight *
                    ageWeight *
                    (1.0F +
                     std::max(
                         slot.
                             invalidationPriorityBoost,
                         0.0F)),
                .ageSeconds =
                    age
            });
        }
    }

    std::stable_sort(
        candidates.begin(),
        candidates.end(),
        [](const auto& a,
           const auto& b)
        {
            if (a.priority != b.priority)
            {
                return
                    a.priority >
                    b.priority;
            }

            return
                std::tie(
                    a.key.level,
                    a.key.x,
                    a.key.y,
                    a.key.z) <
                std::tie(
                    b.key.level,
                    b.key.x,
                    b.key.y,
                    b.key.z);
        });

    if (candidates.size() >
        maximumUpdates)
    {
        candidates.resize(
            maximumUpdates);
    }

    return candidates;
}

bool RadianceClipmapResidency::CommitUpdate(
    const RadianceCellKey& key,
    const DirectionalIrradianceL1& irradiance,
    const u32 sampleCount,
    const u64 sourceRevision)
{
    if (sourceRevision !=
            sourceRevision_ ||
        !BelongsToCurrentWindow(key))
    {
        return false;
    }

    auto* slot =
        SlotFor(key);

    if (slot == nullptr ||
        !slot->occupied ||
        slot->key != key)
    {
        return false;
    }

    slot->cell.irradiance =
        irradiance;
    slot->cell.revision =
        sourceRevision;
    slot->cell.updateAgeSeconds =
        0.0F;
    slot->cell.sampleCount =
        sampleCount;
    slot->cell.valid =
        true;

    slot->sourceRevision =
        sourceRevision;
    slot->invalidationPriorityBoost =
        0.0F;
    slot->dirty = false;

    return true;
}

const RadianceResidentCell*
RadianceClipmapResidency::Lookup(
    const RadianceCellKey& key,
    const u64 requiredRevision) const noexcept
{
    if (!BelongsToCurrentWindow(key))
    {
        return nullptr;
    }

    const auto* slot =
        SlotFor(key);

    if (slot == nullptr ||
        !slot->occupied ||
        slot->key != key ||
        slot->dirty ||
        !slot->cell.valid ||
        slot->sourceRevision !=
            requiredRevision ||
        slot->cell.revision !=
            requiredRevision)
    {
        return nullptr;
    }

    return slot;
}

RadianceGpuSnapshot
RadianceClipmapResidency::BuildGpuSnapshot(
    const LightingView& view) const
{
    if (view.frame != frame_ ||
        view.body != body_)
    {
        throw std::invalid_argument(
            "Radiance GPU snapshot view does not match residency authority.");
    }

    RadianceGpuSnapshot snapshot;
    snapshot.sourceRevision =
        sourceRevision_;
    snapshot.levels.reserve(
        levels_.size());

    u32 cellOffset = 0U;

    for (const auto& level :
         levels_)
    {
        const RadianceCellKey centerKey{
            .frame = frame_,
            .body = body_,
            .level = level.level,
            .x = level.centerX,
            .y = level.centerY,
            .z = level.centerZ
        };

        const auto centerGpu =
            RadianceCellGpuCenter(
                centerKey,
                config_,
                view);

        const u32 levelCellCount =
            static_cast<u32>(
                level.slots.size());

        snapshot.levels.push_back({
            .centerCellSize = {
                centerGpu.x,
                centerGpu.y,
                centerGpu.z,
                static_cast<f32>(
                    RadianceCellSizeMeters(
                        config_,
                        level.level))
            },
            .centerModuloX =
                PositiveModulo(
                    level.centerX,
                    config_.cellsPerAxis),
            .centerModuloY =
                PositiveModulo(
                    level.centerY,
                    config_.cellsPerAxis),
            .centerModuloZ =
                PositiveModulo(
                    level.centerZ,
                    config_.cellsPerAxis),
            .cellsPerAxis =
                config_.cellsPerAxis,
            .cellOffset =
                cellOffset,
            .cellCount =
                levelCellCount,
            .level =
                level.level
        });

        snapshot.cells.reserve(
            snapshot.cells.size() +
            level.slots.size());

        for (const auto& slot :
             level.slots)
        {
            RadianceCell exportCell =
                slot.cell;

            const bool current =
                slot.occupied &&
                BelongsToCurrentWindow(
                    slot.key) &&
                slot.cell.valid &&
                slot.sourceRevision ==
                    sourceRevision_ &&
                slot.cell.revision ==
                    sourceRevision_;

            exportCell.valid =
                current;

            snapshot.cells.push_back(
                EncodeGpuRadianceCell(
                    exportCell));
        }

        if (cellOffset >
            std::numeric_limits<u32>::max() -
                levelCellCount)
        {
            throw std::overflow_error(
                "Radiance GPU snapshot exceeds 32-bit cell indexing.");
        }

        cellOffset +=
            levelCellCount;
    }

    return snapshot;
}

RadianceResidencyStats
RadianceClipmapResidency::Stats() const noexcept
{
    RadianceResidencyStats stats;

    for (const auto& level :
         levels_)
    {
        for (const auto& slot :
             level.slots)
        {
            if (!slot.occupied ||
                !BelongsToCurrentWindow(
                    slot.key))
            {
                continue;
            }

            ++stats.residentCells;

            if (slot.dirty ||
                !slot.cell.valid ||
                slot.sourceRevision !=
                    sourceRevision_)
            {
                ++stats.dirtyCells;
            }
        }
    }

    return stats;
}

const RadianceClipmapConfig&
RadianceClipmapResidency::Config() const noexcept
{
    return config_;
}
} // namespace orbit::lighting
