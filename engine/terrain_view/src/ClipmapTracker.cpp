#include <orbit/terrain_view/ClipmapTracker.hpp>

#include <cmath>
#include <stdexcept>

namespace orbit::terrain_view
{
namespace
{
// Keep the exponential-map chart bounded so float shader coordinates stay
// precise and spherical distortion remains small. A rebase is deliberately
// rare: on a 6000 km planet this is about 150 km of travel, versus ordinary
// toroidal strip updates every sample cell.
constexpr f64 kLatticeRebaseAngleRadians = 0.025;
} // namespace

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

    if (!latticeInitialized_)
    {
        latticeFrame_ =
            world::MakeSurfaceFrame(
                observerDirection);
        latticeInitialized_ = true;
    }

    math::Double2 observerOffset =
        world::SurfaceOffsetBetweenDirections(
            planet_,
            latticeFrame_,
            observerDirection);

    const f64 rebaseDistanceMeters =
        planet_.radiusMeters *
        kLatticeRebaseAngleRadians;

    const bool rebase =
        observerOffset.x * observerOffset.x +
            observerOffset.y * observerOffset.y >
        rebaseDistanceMeters *
            rebaseDistanceMeters;

    if (rebase)
    {
        // Re-anchor the whole LOD stack at once. Between these rare rebases
        // every level lives on one immutable spherical integer lattice, which
        // makes toroidal strip reuse exact instead of merely approximate.
        latticeFrame_ =
            world::TransportSurfaceFrameToDirection(
                latticeFrame_,
                observerDirection);

        observerOffset = {};

        for (LevelState& state : levels_)
        {
            if (!state.initialized)
            {
                continue;
            }

            state.centerOffsetMeters = {};
            state.centerDirection =
                observerDirection;
        }
    }

    ClipmapMotionUpdate update{};
    update.levels.resize(levels_.size());

    for (u32 index = 0;
         index < static_cast<u32>(levels_.size());
         ++index)
    {
        LevelState& state =
            levels_[index];

        const ClipmapLevel& level =
            layout.levels[index];

        ClipmapLevelMotion motion{};
        motion.levelIndex = index;
        motion.surfaceFrame = latticeFrame_;

        // Keep each level phase-locked to its parent lattice. A finer level
        // therefore recenters in whole parent-cell increments (two of its own
        // cells for the normal 2:1 clipmap ratio). This is the same structural
        // invariant the working Godot quadtree gets naturally: child borders
        // always land on parent-grid coordinates instead of alternating between
        // aligned and half-cell phases as the observer moves.
        const f64 alignmentSpacingMeters =
            index + 1U < layout.levels.size()
                ? layout.levels[index + 1U].
                    sampleSpacingMeters
                : level.sampleSpacingMeters;

        const math::Double2 desiredCenterOffsetMeters{
            std::round(
                observerOffset.x /
                alignmentSpacingMeters) *
                alignmentSpacingMeters,
            std::round(
                observerOffset.y /
                alignmentSpacingMeters) *
                alignmentSpacingMeters
        };

        if (!state.initialized)
        {
            state.initialized = true;
            state.samplesInvalidated = false;

            state.centerOffsetMeters =
                desiredCenterOffsetMeters;

            state.centerDirection =
                world::DirectionAtSurfaceOffset(
                    planet_,
                    latticeFrame_,
                    state.centerOffsetMeters);

            motion.centerDirection =
                state.centerDirection;
            motion.centerOffsetMeters =
                state.centerOffsetMeters;
            motion.fullRefresh = true;

            update.levels[index] = motion;
            continue;
        }

        if (!rebase)
        {
            const math::Double2 delta{
                desiredCenterOffsetMeters.x -
                    state.centerOffsetMeters.x,
                desiredCenterOffsetMeters.y -
                    state.centerOffsetMeters.y
            };

            const i64 shiftX =
                static_cast<i64>(
                    std::llround(
                        delta.x /
                        level.sampleSpacingMeters));

            const i64 shiftY =
                static_cast<i64>(
                    std::llround(
                        delta.y /
                        level.sampleSpacingMeters));

            motion.cellShiftX = shiftX;
            motion.cellShiftY = shiftY;

            // Assign the snapped target directly instead of accumulating the
            // rounded cell delta. That prevents long-run floating point drift
            // from ever breaking the parent/child phase lock.
            state.centerOffsetMeters =
                desiredCenterOffsetMeters;

            state.centerDirection =
                world::DirectionAtSurfaceOffset(
                    planet_,
                    latticeFrame_,
                    state.centerOffsetMeters);
        }

        motion.centerDirection =
            state.centerDirection;
        motion.centerOffsetMeters =
            state.centerOffsetMeters;

        motion.fullRefresh =
            rebase ||
            state.samplesInvalidated;

        state.samplesInvalidated = false;

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

    latticeInitialized_ = false;
    latticeFrame_ = {};
}

void ClipmapTracker::InvalidateSamples() noexcept
{
    for (LevelState& state : levels_)
    {
        state.samplesInvalidated = true;
    }
}

ClipmapTracker ClipmapTracker::Reconfigured(
    const ClipmapConfig config) const
{
    ClipmapTracker result(
        planet_,
        config);

    result.latticeInitialized_ =
        latticeInitialized_;
    result.latticeFrame_ =
        latticeFrame_;

    const world::WorldPosition observer{
        {planet_.radiusMeters, 0.0, 0.0}
    };

    const auto previous =
        BuildClipmapLayout(
            config_,
            observer);

    const auto next =
        BuildClipmapLayout(
            config,
            observer);

    for (std::size_t target = 0;
         target < result.levels_.size();
         ++target)
    {
        for (std::size_t source = 0;
             source < levels_.size();
             ++source)
        {
            if (next.levels[target].
                    sampleSpacingMeters ==
                previous.levels[source].
                    sampleSpacingMeters)
            {
                // Coverage tier changes remap level indices. Preserve the
                // stable spherical lattice coordinates, but force one refill
                // because the renderer's per-index GPU buffers/residency are
                // rebuilt for the new tier.
                result.levels_[target] =
                    levels_[source];

                result.levels_[target].
                    samplesInvalidated = true;
                break;
            }
        }
    }

    return result;
}

const ClipmapConfig&
ClipmapTracker::Config() const noexcept
{
    return config_;
}
} // namespace orbit::terrain_view
