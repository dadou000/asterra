#include <orbit/terrain_view/ClipmapPlanner.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace orbit::terrain_view
{
namespace
{
constexpr f64 kLog2 = 0.69314718055994530942;

[[nodiscard]] f64 Log2(const f64 value) noexcept
{
    return std::log(value) / kLog2;
}

struct GroundSurvey
{
    bool anyHit{false};
    bool anyMiss{false};
    f64 nearestMeters{0.0};
    f64 farthestArcMeters{0.0};
};

// Intersects a grid of rays across the viewport with the ground sphere.
[[nodiscard]] GroundSurvey SurveyGround(
    const ClipmapPlannerConfig& config,
    const ClipmapPlanView& view,
    const f64 cameraRadius,
    const f64 groundRadius)
{
    GroundSurvey survey;
    survey.nearestMeters = std::numeric_limits<f64>::infinity();

    const f64 forwardLength = math::Length(view.forward);
    if (forwardLength <= 0.0 || !std::isfinite(forwardLength))
    {
        return survey;
    }
    const math::Double3 forward = view.forward * (1.0 / forwardLength);
    math::Double3 right = math::Cross(view.up, forward);
    const f64 rightLength = math::Length(right);
    if (rightLength <= 1.0e-9)
    {
        // Looking along the up vector: use any perpendicular.
        right = math::Cross(
            std::abs(forward.y) < 0.9 ? math::Double3{0.0, 1.0, 0.0}
                                       : math::Double3{1.0, 0.0, 0.0},
            forward);
    }
    right = math::Normalize(right);
    const math::Double3 up = math::Normalize(math::Cross(forward, right));

    const f64 tanVertical = std::tan(view.verticalFovRadians * 0.5);
    const f64 aspect = static_cast<f64>(std::max(view.viewportWidthPixels, 1U)) /
        static_cast<f64>(std::max(view.viewportHeightPixels, 1U));
    const f64 tanHorizontal = tanVertical * aspect;

    const u32 across = std::max(config.raysAcross, 2U);
    const u32 down = std::max(config.raysDown, 2U);
    const f64 cConstant = cameraRadius * cameraRadius - groundRadius * groundRadius;

    for (u32 iy = 0; iy < down; ++iy)
    {
        const f64 ndcY = -1.0 + 2.0 * static_cast<f64>(iy) /
            static_cast<f64>(down - 1U);
        for (u32 ix = 0; ix < across; ++ix)
        {
            const f64 ndcX = -1.0 + 2.0 * static_cast<f64>(ix) /
                static_cast<f64>(across - 1U);
            const math::Double3 direction = math::Normalize(
                forward + right * (ndcX * tanHorizontal) + up * (ndcY * tanVertical));

            const f64 b = math::Dot(view.position, direction);
            const f64 discriminant = b * b - cConstant;
            if (discriminant < 0.0 || b >= 0.0)
            {
                survey.anyMiss = true;
                continue;
            }
            const f64 t = -b - std::sqrt(discriminant);
            if (t <= 0.0)
            {
                survey.anyMiss = true;
                continue;
            }

            const math::Double3 point = view.position + direction * t;
            const f64 pointLength = math::Length(point);
            const f64 cosine = std::clamp(
                math::Dot(view.position, point) / (cameraRadius * pointLength),
                -1.0,
                1.0);
            const f64 arc = groundRadius * std::acos(cosine);

            survey.anyHit = true;
            survey.nearestMeters = std::min(survey.nearestMeters, t);
            survey.farthestArcMeters = std::max(survey.farthestArcMeters, arc);
        }
    }
    return survey;
}
} // namespace

void ClipmapPlanner::SetConfig(const ClipmapPlannerConfig& config) noexcept
{
    config_ = config;
    config_.pixelsPerVertex = std::clamp(config_.pixelsPerVertex, 0.25, 64.0);
    config_.hysteresisOctaves = std::clamp(config_.hysteresisOctaves, 0.0, 0.9);
    config_.coverageMargin = std::clamp(config_.coverageMargin, 1.0, 4.0);
    config_.reliefMarginMeters = std::max(config_.reliefMarginMeters, 0.0);
    config_.raysAcross = std::clamp(config_.raysAcross, 2U, 64U);
    config_.raysDown = std::clamp(config_.raysDown, 2U, 64U);
    havePrevious_ = false;
}

ClipmapPlan ClipmapPlanner::Plan(
    const ClipmapConfig& ladder,
    const ClipmapPlanView& view)
{
    if (ladder.gridResolution < 9U || (ladder.gridResolution % 2U) == 0U ||
        ClipmapLevelCount(ladder) == 0U ||
        (!ladder.Banded() &&
         (ladder.levelCount == 0U || ladder.baseSpacingMeters <= 0.0)))
    {
        throw std::invalid_argument(
            "Orbit clipmap planner needs a valid clipmap ladder.");
    }

    const u32 levelCount = ClipmapLevelCount(ladder);
    const u32 maxLevel = levelCount - 1U;
    const f64 base = ladder.Banded()
        ? ClipmapLevelSpacingMeters(ladder, 0U)
        : ladder.baseSpacingMeters;
    // Half extent of one ladder level; coarse levels can carry a denser grid.
    const auto extentOf = [&ladder](const u32 level)
    {
        return ClipmapLevelHalfExtentMeters(ladder, level);
    };

    ClipmapPlan plan;
    plan.finestSpacingMeters = base;
    plan.coarsestHalfExtentMeters = extentOf(maxLevel);

    if (!config_.enabled)
    {
        plan.dynamic = false;
        plan.firstLevel = 0U;
        plan.lastLevel = maxLevel;
        last_ = plan;
        havePrevious_ = false;
        return plan;
    }
    plan.dynamic = true;

    const f64 cameraRadius = math::Length(view.position);
    if (!std::isfinite(cameraRadius) || cameraRadius <= 0.0 ||
        view.planetRadiusMeters <= 0.0)
    {
        // No usable pose: keep everything rather than guess.
        plan.dynamic = false;
        plan.firstLevel = 0U;
        plan.lastLevel = maxLevel;
        last_ = plan;
        return plan;
    }

    // The ground sphere includes the relief margin so terrain standing above the
    // sample under the camera is not assumed farther away than it can be.
    const f64 groundRadius = std::min(
        view.planetRadiusMeters + view.groundElevationMeters +
            config_.reliefMarginMeters,
        cameraRadius - 0.5);
    const f64 nadirDistance = std::max(cameraRadius - groundRadius, 0.5);

    const GroundSurvey survey =
        SurveyGround(config_, view, cameraRadius, groundRadius);
    plan.groundInView = survey.anyHit;

    f64 nearest = nadirDistance;
    // Always cover the ground below, as far as 1.5x the height above it.
    f64 arc = nadirDistance * 1.5;
    if (survey.anyHit)
    {
        nearest = std::min(nearest, survey.nearestMeters);
        arc = std::max(arc, survey.farthestArcMeters);
        if (survey.anyMiss)
        {
            // Some rays miss the ground: the horizon is in view.
            const f64 ratio = std::clamp(groundRadius / cameraRadius, 0.0, 1.0);
            arc = std::max(arc, groundRadius * std::acos(ratio));
        }
    }
    // How far the ground can be seen is a different question from how near it is.
    // The ground sphere above is the terrain around the camera (its height hint
    // plus the relief margin), which is right for the NEAREST ground but wrong
    // for the farthest: on a mountain it sits at the camera's own height and
    // collapses the coverage to a few hundred metres, although the land and sea
    // far below stay in view. Far coverage is sized from the mean planet radius.
    f64 farRadius = groundRadius;
    if (groundRadius > view.planetRadiusMeters &&
        view.planetRadiusMeters < cameraRadius - 0.5)
    {
        farRadius = view.planetRadiusMeters;
        const GroundSurvey farSurvey =
            SurveyGround(config_, view, cameraRadius, farRadius);
        if (farSurvey.anyHit)
        {
            arc = std::max(arc, farSurvey.farthestArcMeters);
            if (farSurvey.anyMiss)
            {
                const f64 farRatio = std::clamp(farRadius / cameraRadius, 0.0, 1.0);
                arc = std::max(arc, farRadius * std::acos(farRatio));
            }
        }
    }
    plan.nearestGroundMeters = nearest;
    plan.visibleArcMeters = arc;
    {
        // Straight-line distance to the farthest visible ground: the chord of the
        // ground sphere between the camera and a point `arc` away from the nadir.
        const f64 angle = std::min(arc / farRadius, 3.14159265358979);
        plan.farthestDistanceMeters = std::sqrt(std::max(
            cameraRadius * cameraRadius + farRadius * farRadius -
                2.0 * cameraRadius * farRadius * std::cos(angle),
            0.0));
        plan.farthestDistanceMeters =
            std::max(plan.farthestDistanceMeters, nearest);
    }

    if (ladder.Banded())
    {
        // Distance bands: level k is drawn where the camera's distance to the
        // ground lies in its band (plus the cross-fade zone), so the levels
        // wanted are those whose band overlaps [nearest, farthest]. Coverage
        // changes continuously with distance, so a level is added while it is
        // still invisible and no hysteresis is needed; a small margin lets it be
        // generated before it shows.
        constexpr f64 kMargin = 1.1;
        const f64 zone = ladder.bandZoneFraction;
        u32 firstBand = 0U;
        u32 lastBand = maxLevel;
        for (u32 level = 0U; level <= maxLevel; ++level)
        {
            const ClipmapBand band = ClipmapLevelBand(ladder, level);
            if (band.outerMeters * (1.0 + zone) * kMargin >= nearest)
            {
                firstBand = level;
                break;
            }
            firstBand = level;
        }
        for (u32 level = maxLevel + 1U; level-- > 0U;)
        {
            const ClipmapBand band = ClipmapLevelBand(ladder, level);
            lastBand = level;
            if (band.innerMeters * (1.0 - zone) / kMargin <=
                plan.farthestDistanceMeters)
            {
                break;
            }
        }
        lastBand = std::max(lastBand, firstBand);
        plan.firstLevel = firstBand;
        plan.lastLevel = lastBand;
        plan.finestSpacingMeters = ClipmapLevelSpacingMeters(ladder, firstBand);
        plan.coarsestHalfExtentMeters = extentOf(lastBand);
        last_ = plan;
        havePrevious_ = true;
        return plan;
    }

    // ---- finest level ---------------------------------------------------------
    const f64 pixelAngle = 2.0 * std::tan(view.verticalFovRadians * 0.5) /
        static_cast<f64>(std::max(view.viewportHeightPixels, 1U));
    const f64 required = config_.pixelsPerVertex * pixelAngle * nearest;
    plan.requiredSpacingMeters = required;
    // Octaves above the ladder's base spacing.
    const f64 x = required > base ? Log2(required / base) : -1.0;

    u32 first = x < 0.0 ? 0U : static_cast<u32>(std::floor(x));
    if (havePrevious_ && last_.dynamic)
    {
        u32 held = std::min(last_.firstLevel, maxLevel);
        if (x < static_cast<f64>(held))
        {
            // Demand reached a finer level: add it at once.
            held = x < 0.0 ? 0U : static_cast<u32>(std::floor(x));
        }
        else
        {
            // Drop a fine level only once the demand is clearly past it.
            while (held < maxLevel &&
                   x >= static_cast<f64>(held) + 1.0 + config_.hysteresisOctaves)
            {
                ++held;
            }
        }
        first = held;
    }
    first = std::min(first, maxLevel);

    // ---- coarsest level -------------------------------------------------------
    const f64 demandedExtent = arc * config_.coverageMargin;
    u32 lastLevel = 0U;
    while (lastLevel < maxLevel && extentOf(lastLevel) < demandedExtent)
    {
        ++lastLevel;
    }
    if (havePrevious_ && last_.dynamic)
    {
        u32 held = std::min(last_.lastLevel, maxLevel);
        if (demandedExtent > extentOf(held))
        {
            // Visible ground reaches past the coarsest level: grow at once.
            held = lastLevel;
        }
        else
        {
            // Shrink only once the demand is clearly inside the level below.
            const f64 shrinkFactor = std::exp2(-config_.hysteresisOctaves);
            while (held > 0U &&
                   demandedExtent <= extentOf(held - 1U) * shrinkFactor)
            {
                --held;
            }
        }
        lastLevel = held;
    }
    lastLevel = std::min(lastLevel, maxLevel);

    // ---- keep a usable range --------------------------------------------------
    const u32 minimumLevels = std::clamp(config_.minimumLevels, 1U, ladder.levelCount);
    if (lastLevel < first)
    {
        lastLevel = first;
    }
    if (lastLevel - first + 1U < minimumLevels)
    {
        lastLevel = std::min(first + minimumLevels - 1U, maxLevel);
        if (lastLevel - first + 1U < minimumLevels)
        {
            first = lastLevel + 1U >= minimumLevels ? lastLevel + 1U - minimumLevels : 0U;
        }
    }

    plan.firstLevel = first;
    plan.lastLevel = lastLevel;
    plan.finestSpacingMeters = base * std::pow(2.0, static_cast<f64>(first));
    plan.coarsestHalfExtentMeters = extentOf(lastLevel);

    last_ = plan;
    havePrevious_ = true;
    return plan;
}
} // namespace orbit::terrain_view
