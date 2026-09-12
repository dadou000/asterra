#include <orbit/terrain_view/ClipmapTracker.hpp>

#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace orbit::terrain_view
{
ClipmapTracker::ClipmapTracker(
    const world::PlanetDefinition planet,
    const ClipmapConfig config)
    : planet_(planet),
      config_(config),
      levels_(config.levelCount)
{
    if (planet_.radiusMeters <= 0.0)
    {
        throw std::invalid_argument(
            "Orbit terrain clipmap tracker requires a positive planet radius.");
    }

    const world::WorldPosition validationObserver{
        .meters = {planet_.radiusMeters, 0.0, 0.0}
    };

    static_cast<void>(
        BuildClipmapLayout(config_, validationObserver));
}

ClipmapMotionUpdate ClipmapTracker::Update(
    const world::WorldPosition& observer)
{
    const math::Double3 observerDirection =
        math::Normalize(observer.meters);

    if (math::LengthSquared(observerDirection) <= 0.0)
    {
        throw std::invalid_argument(
            "Orbit terrain clipmap observer cannot be at the planet center.");
    }

    const ClipmapLayout layout =
        BuildClipmapLayout(config_, observer);

    ClipmapMotionUpdate update{};
    update.levels.reserve(levels_.size());

    for (u32 index = 0;
         index < static_cast<u32>(levels_.size());
         ++index)
    {
        LevelState& state = levels_[index];
        const ClipmapLevel& level = layout.levels[index];

        ClipmapLevelMotion motion{};
        motion.levelIndex = index;

        if (!state.initialized)
        {
            state.initialized = true;
            state.centerDirection = observerDirection;
            state.frame =
                world::MakeSurfaceFrame(observerDirection);

            motion.centerDirection = state.centerDirection;
            motion.surfaceFrame = state.frame;
            motion.fullRefresh = true;
            update.levels.push_back(motion);
            continue;
        }

        const math::Double2 offset =
            world::SurfaceOffsetBetweenDirections(
                planet_,
                state.frame,
                observerDirection);

        const i64 shiftX = static_cast<i64>(
            std::llround(
                offset.x / level.sampleSpacingMeters));

        const i64 shiftY = static_cast<i64>(
            std::llround(
                offset.y / level.sampleSpacingMeters));

        motion.cellShiftX = shiftX;
        motion.cellShiftY = shiftY;

        if (shiftX != 0 || shiftY != 0)
        {
            const math::Double2 snappedOffset{
                static_cast<f64>(shiftX) *
                    level.sampleSpacingMeters,
                static_cast<f64>(shiftY) *
                    level.sampleSpacingMeters
            };

            state.frame =
                world::SurfaceFrameAtOffset(
                    planet_,
                    state.frame,
                    snappedOffset);

            state.centerDirection = state.frame.up;
        }

        const i64 gridSize =
            static_cast<i64>(level.gridResolution);

        motion.fullRefresh =
            std::abs(shiftX) >= gridSize ||
            std::abs(shiftY) >= gridSize;

        motion.centerDirection = state.centerDirection;
        motion.surfaceFrame = state.frame;

        update.levels.push_back(motion);
    }

    return update;
}

void ClipmapTracker::Reset() noexcept
{
    for (LevelState& state : levels_)
    {
        state = {};
    }
}

const ClipmapConfig& ClipmapTracker::Config() const noexcept
{
    return config_;
}
} // namespace orbit::terrain_view
