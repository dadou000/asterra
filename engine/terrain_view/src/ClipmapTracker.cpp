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
    update.levels.resize(levels_.size());

    // Frame orientation is hierarchical from coarse to fine. This removes the
    // path-dependent relative roll (spherical holonomy) that accumulated when
    // every LOD independently parallel-transported its own frame. A fine frame
    // is always the direct transport of its immediate coarse parent to the
    // fine center. When a parent frame changes, descendants are regenerated
    // because their local coordinate basis changed even if their centers did
    // not cross a cell boundary.
    std::vector<bool> frameChanged(
        levels_.size(),
        false);

    for (std::size_t reverse = levels_.size();
         reverse > 0;
         --reverse)
    {
        const u32 index =
            static_cast<u32>(
                reverse - 1U);

        LevelState& state = levels_[index];
        const ClipmapLevel& level = layout.levels[index];

        ClipmapLevelMotion motion{};
        motion.levelIndex = index;

        const bool hasParent =
            index + 1U <
            static_cast<u32>(
                levels_.size());

        if (!state.initialized)
        {
            const bool hasResidentChild =
                index > 0U &&
                levels_[index - 1U].initialized;

            state.initialized = true;
            state.samplesInvalidated = false;
            state.centerDirection = observerDirection;

            if (hasParent)
            {
                state.frame =
                    world::TransportSurfaceFrameToDirection(
                        levels_[index + 1U].frame,
                        observerDirection);
            }
            else if (hasResidentChild)
            {
                // Adaptive coverage can introduce one new coarsest level
                // while all finer spacings are preserved. Orient that new
                // parent by reverse-transporting the existing child frame so
                // transporting it back to the child's center reproduces the
                // resident child basis instead of rotating every shared grid.
                state.frame =
                    world::TransportSurfaceFrameToDirection(
                        levels_[index - 1U].frame,
                        observerDirection);
            }
            else
            {
                state.frame =
                    world::MakeSurfaceFrame(
                        observerDirection);
            }

            // A newly introduced coarsest parent constructed from a resident
            // child has no old samples of its own, but it also does not change
            // that child's coordinate frame. Do not propagate a false frame
            // change down the preserved hierarchy.
            frameChanged[index] =
                !( !hasParent &&
                   hasResidentChild );

            motion.centerDirection = state.centerDirection;
            motion.surfaceFrame = state.frame;
            motion.fullRefresh = true;
            update.levels[index] = motion;
            continue;
        }

        const bool parentFrameChanged =
            hasParent &&
            frameChanged[index + 1U];

        if (parentFrameChanged)
        {
            state.frame =
                world::TransportSurfaceFrameToDirection(
                    levels_[index + 1U].frame,
                    state.centerDirection);
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

        const bool centerMoved =
            shiftX != 0 ||
            shiftY != 0;

        if (centerMoved)
        {
            const math::Double2 snappedOffset{
                static_cast<f64>(shiftX) *
                    level.sampleSpacingMeters,
                static_cast<f64>(shiftY) *
                    level.sampleSpacingMeters
            };

            const world::SurfaceFrame movedFrame =
                world::SurfaceFrameAtOffset(
                    planet_,
                    state.frame,
                    snappedOffset);

            state.centerDirection = movedFrame.up;
            state.frame =
                hasParent
                    ? world::TransportSurfaceFrameToDirection(
                        levels_[index + 1U].frame,
                        state.centerDirection)
                    : movedFrame;
        }

        frameChanged[index] =
            parentFrameChanged ||
            centerMoved;

        // A toroidal strip reuse is only exact on a flat, translation-
        // invariant lattice. On the sphere, moving or rotating the tangent
        // frame changes the world-space address of every retained logical
        // sample. Regenerate the whole level until the clipmap is backed by a
        // stable spherical integer lattice.
        motion.fullRefresh =
            state.samplesInvalidated ||
            frameChanged[index];

        state.samplesInvalidated = false;

        motion.centerDirection = state.centerDirection;
        motion.surfaceFrame = state.frame;

        update.levels[index] = motion;
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

void ClipmapTracker::InvalidateSamples() noexcept
{
    for (LevelState& state : levels_)
    {
        state.samplesInvalidated = true;
    }
}

ClipmapTracker ClipmapTracker::Reconfigured(const ClipmapConfig config) const
{
    ClipmapTracker result(planet_, config);
    const world::WorldPosition observer{{planet_.radiusMeters, 0.0, 0.0}};
    const auto previous = BuildClipmapLayout(config_, observer);
    const auto next = BuildClipmapLayout(config, observer);
    for (std::size_t target = 0; target < result.levels_.size(); ++target)
    {
        for (std::size_t source = 0; source < levels_.size(); ++source)
        {
            if (next.levels[target].sampleSpacingMeters == previous.levels[source].sampleSpacingMeters)
            {
                // Changing coverage shifts level indices, not world-space
                // sample locations for resolutions common to both layouts.
                result.levels_[target] = levels_[source];
                result.levels_[target].samplesInvalidated = true;
                break;
            }
        }
    }
    return result;
}

const ClipmapConfig& ClipmapTracker::Config() const noexcept
{
    return config_;
}
} // namespace orbit::terrain_view
