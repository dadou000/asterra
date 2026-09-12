#include <orbit/terrain_stream/ToroidalResidency.hpp>

#include <algorithm>
#include <cstdlib>
#include <stdexcept>

namespace orbit::terrain_stream
{
namespace
{
[[nodiscard]] u32 WrapIndex(
    const i64 value,
    const u32 size) noexcept
{
    const i64 modulus =
        static_cast<i64>(size);

    i64 wrapped =
        value % modulus;

    if (wrapped < 0)
    {
        wrapped += modulus;
    }

    return static_cast<u32>(wrapped);
}

template <typename Emit>
void EmitWrappedSpan(
    const u32 physicalStart,
    const u32 length,
    const u32 size,
    Emit&& emit)
{
    if (length == 0)
    {
        return;
    }

    const u32 firstLength =
        std::min(
            length,
            size - physicalStart);

    emit(
        physicalStart,
        firstLength);

    const u32 remaining =
        length - firstLength;

    if (remaining > 0)
    {
        emit(0, remaining);
    }
}
} // namespace

ToroidalResidency::ToroidalResidency(
    const terrain_view::ClipmapConfig config)
    : config_(config),
      levels_(config.levelCount)
{
    if (config_.levelCount == 0)
    {
        throw std::invalid_argument(
            "Orbit toroidal residency requires at least one clipmap level.");
    }

    if (config_.gridResolution < 2)
    {
        throw std::invalid_argument(
            "Orbit toroidal residency requires at least two samples per axis.");
    }
}

ResidencyUpdate ToroidalResidency::Apply(
    const terrain_view::ClipmapMotionUpdate& motion)
{
    if (motion.levels.size() !=
        levels_.size())
    {
        throw std::invalid_argument(
            "Orbit toroidal residency motion level count does not match its configuration.");
    }

    const u32 resolution =
        config_.gridResolution;

    ResidencyUpdate update{};
    update.levels.reserve(
        levels_.size());

    for (u32 levelIndex = 0;
         levelIndex <
            static_cast<u32>(
                levels_.size());
         ++levelIndex)
    {
        const auto& movement =
            motion.levels[levelIndex];

        if (movement.levelIndex !=
            levelIndex)
        {
            throw std::invalid_argument(
                "Orbit toroidal residency received out-of-order clipmap motion.");
        }

        LevelState& state =
            levels_[levelIndex];

        LevelResidencyUpdate levelUpdate{};
        levelUpdate.levelIndex =
            levelIndex;

        const i64 shiftX =
            movement.cellShiftX;
        const i64 shiftY =
            movement.cellShiftY;

        const bool tooLarge =
            std::abs(shiftX) >=
                static_cast<i64>(
                    resolution) ||
            std::abs(shiftY) >=
                static_cast<i64>(
                    resolution);

        if (movement.fullRefresh ||
            tooLarge)
        {
            state.originX = 0;
            state.originY = 0;

            levelUpdate.originX = 0;
            levelUpdate.originY = 0;
            levelUpdate.fullRefresh = true;
            levelUpdate.refreshRegions.push_back({
                .x = 0,
                .y = 0,
                .width = resolution,
                .height = resolution
            });

            update.levels.push_back(
                std::move(levelUpdate));

            continue;
        }

        state.originX =
            WrapIndex(
                static_cast<i64>(
                    state.originX) +
                shiftX,
                resolution);

        state.originY =
            WrapIndex(
                static_cast<i64>(
                    state.originY) +
                shiftY,
                resolution);

        levelUpdate.originX =
            state.originX;

        levelUpdate.originY =
            state.originY;

        if (shiftX != 0)
        {
            const u32 width =
                static_cast<u32>(
                    std::abs(shiftX));

            const u32 logicalStart =
                shiftX > 0
                    ? resolution - width
                    : 0;

            const u32 physicalStart =
                WrapIndex(
                    static_cast<i64>(
                        logicalStart) +
                    static_cast<i64>(
                        state.originX),
                    resolution);

            EmitWrappedSpan(
                physicalStart,
                width,
                resolution,
                [&levelUpdate, resolution](
                    const u32 start,
                    const u32 span)
                {
                    levelUpdate.
                        refreshRegions.
                        push_back({
                            .x = start,
                            .y = 0,
                            .width = span,
                            .height =
                                resolution
                        });
                });
        }

        if (shiftY != 0)
        {
            const u32 height =
                static_cast<u32>(
                    std::abs(shiftY));

            const u32 logicalYStart =
                shiftY > 0
                    ? resolution - height
                    : 0;

            const u32 physicalYStart =
                WrapIndex(
                    static_cast<i64>(
                        logicalYStart) +
                    static_cast<i64>(
                        state.originY),
                    resolution);

            // When both axes move, the newly exposed X strip already
            // refreshes the diagonal corner. Refresh only the logical X
            // complement for the Y strip so expensive terrain sampling is
            // never scheduled twice for the same physical cell.
            const u32 xRefreshWidth =
                shiftX != 0
                    ? static_cast<u32>(
                        std::abs(shiftX))
                    : 0U;

            const u32 logicalXStart =
                shiftX < 0
                    ? xRefreshWidth
                    : 0U;

            const u32 logicalXLength =
                resolution -
                xRefreshWidth;

            const u32 physicalXStart =
                WrapIndex(
                    static_cast<i64>(
                        logicalXStart) +
                    static_cast<i64>(
                        state.originX),
                    resolution);

            EmitWrappedSpan(
                physicalYStart,
                height,
                resolution,
                [&levelUpdate,
                 resolution,
                 physicalXStart,
                 logicalXLength](
                    const u32 yStart,
                    const u32 ySpan)
                {
                    EmitWrappedSpan(
                        physicalXStart,
                        logicalXLength,
                        resolution,
                        [&levelUpdate,
                         yStart,
                         ySpan](
                            const u32 xStart,
                            const u32 xSpan)
                        {
                            levelUpdate.
                                refreshRegions.
                                push_back({
                                    .x = xStart,
                                    .y = yStart,
                                    .width = xSpan,
                                    .height = ySpan
                                });
                        });
                });
        }

        update.levels.push_back(
            std::move(levelUpdate));
    }

    return update;
}

void ToroidalResidency::Reset() noexcept
{
    for (LevelState& state :
         levels_)
    {
        state = {};
    }
}

const terrain_view::ClipmapConfig&
ToroidalResidency::Config() const noexcept
{
    return config_;
}
} // namespace orbit::terrain_stream
