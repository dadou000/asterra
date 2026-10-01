#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain_view/ClipmapLayout.hpp>

namespace orbit::terrain_view
{
// Which levels of the clipmap ladder a camera actually needs.
//
// The ladder is a fixed 2:1 octave lattice (level j has spacing base * 2^j), so
// a level that stays active keeps its resident samples whatever the camera does.
// The plan only chooses a contiguous range of it:
//
//  * finest: levels whose sample spacing is much finer than one screen pixel at
//    the NEAREST visible ground are wasted work, so they are dropped. Looking
//    straight down from 10 km the finest levels (1 m, 2 m, ...) disappear.
//  * coarsest: levels beyond the visible ground are not drawn. Looking straight
//    down from 1 km needs about 2 km of coverage, not the 130 km the full ladder
//    reaches; looking at the horizon needs the horizon arc.
//
// The ground directly below the camera always counts as visible, so turning the
// camera does not suddenly need levels that were just dropped.
struct ClipmapPlannerConfig
{
    bool enabled{true};
    // Target sample spacing in screen pixels at the nearest visible ground.
    // Lower keeps finer levels active longer (more detail, more work).
    f64 pixelsPerVertex{3.0};
    // How far past a level boundary (in octaves) the demand must go before a
    // level is added or removed, so a camera sitting on a boundary does not
    // make levels flicker. A level is added as soon as the demand reaches it.
    f64 hysteresisOctaves{0.4};
    // Outer coverage kept beyond the farthest visible ground.
    f64 coverageMargin{1.2};
    // Terrain may stand this much above the height sample under the camera, so
    // the nearest ground can be closer than the nadir estimate says.
    f64 reliefMarginMeters{300.0};
    // At least this many levels stay active (clamped to the ladder).
    u32 minimumLevels{3};
    // Viewport sampling density used to find the nearest and farthest ground.
    u32 raysAcross{9};
    u32 raysDown{7};

    [[nodiscard]] bool operator==(const ClipmapPlannerConfig&) const noexcept = default;
};

struct ClipmapPlanView
{
    // Planet-centred position and orthonormal view axes.
    math::Double3 position{};
    math::Double3 forward{0.0, 0.0, 1.0};
    math::Double3 up{0.0, 1.0, 0.0};
    f64 verticalFovRadians{1.2217};
    u32 viewportWidthPixels{1920};
    u32 viewportHeightPixels{1080};
    f64 planetRadiusMeters{6'371'000.0};
    // Terrain elevation under the camera (above the planet radius).
    f64 groundElevationMeters{0.0};
};

struct ClipmapPlan
{
    // False when the planner is disabled: every ladder level is active.
    bool dynamic{false};
    // Inclusive indices into the ladder.
    u32 firstLevel{0};
    u32 lastLevel{0};

    // Slant distance to the nearest visible ground, including the ground below.
    f64 nearestGroundMeters{0.0};
    // Surface arc from the point below the camera to the farthest visible ground.
    f64 visibleArcMeters{0.0};
    // Spacing the screen asks for at the nearest ground (before quantising).
    f64 requiredSpacingMeters{0.0};
    // Sample spacing of the finest active level, and the half extent of the
    // coarsest one.
    f64 finestSpacingMeters{0.0};
    f64 coarsestHalfExtentMeters{0.0};
    // True when the view showed ground; otherwise only the ground below counts.
    bool groundInView{false};
    // Straight-line distance from the camera to the farthest visible ground.
    f64 farthestDistanceMeters{0.0};

    [[nodiscard]] u32 ActiveLevels() const noexcept
    {
        return lastLevel >= firstLevel ? lastLevel - firstLevel + 1U : 0U;
    }
    [[nodiscard]] bool Active(const u32 level) const noexcept
    {
        return level >= firstLevel && level <= lastLevel;
    }
    [[nodiscard]] bool operator==(const ClipmapPlan& other) const noexcept
    {
        return firstLevel == other.firstLevel && lastLevel == other.lastLevel &&
               dynamic == other.dynamic;
    }
};

class ClipmapPlanner
{
public:
    explicit ClipmapPlanner(ClipmapPlannerConfig config = {}) noexcept
        : config_(config)
    {
    }

    void SetConfig(const ClipmapPlannerConfig& config) noexcept;
    [[nodiscard]] const ClipmapPlannerConfig& Config() const noexcept
    {
        return config_;
    }

    // Forgets the previous plan (the next one is built without hysteresis).
    void Reset() noexcept { havePrevious_ = false; }

    // Computes the plan for `view` and stores it as the previous one for
    // hysteresis. `ladder` is the full ladder (level count, grid resolution, base
    // spacing); the plan never leaves it.
    [[nodiscard]] ClipmapPlan Plan(
        const ClipmapConfig& ladder,
        const ClipmapPlanView& view);

    [[nodiscard]] const ClipmapPlan& Last() const noexcept { return last_; }

private:
    ClipmapPlannerConfig config_;
    ClipmapPlan last_{};
    bool havePrevious_{false};
};
} // namespace orbit::terrain_view
