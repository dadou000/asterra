#include <orbit/studio_ui/StudioSurfacePicking.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace orbit::studio_ui
{
namespace
{
[[nodiscard]] f64 PixelFootprintMeters(
    const render_view::CameraState& camera,
    const u32 height,
    const f64 distanceMeters) noexcept
{
    if (height == 0U ||
        !std::isfinite(distanceMeters) ||
        distanceMeters <= 0.0)
    {
        return 1.0;
    }

    const f64 halfFov =
        std::clamp(
            static_cast<f64>(
                camera.verticalFovRadians) *
                0.5,
            1.0e-4,
            1.55);

    const f64 viewportHeight =
        2.0 *
        distanceMeters *
        std::tan(halfFov);

    return
        std::max(
            viewportHeight /
                static_cast<f64>(height),
            0.25);
}

[[nodiscard]] bool RuntimeMatchesViewport(
    const studio_session::ViewportTargetState& viewport,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime) noexcept
{
    return
        viewport.target.has_value() &&
        viewport.target->body ==
            runtime.body &&
        viewport.target->semanticObject ==
            runtime.semanticBody &&
        viewport.target->universeGeneration ==
            runtime.universeGeneration &&
        runtime.planet.id.IsValid() &&
        std::isfinite(
            runtime.planet.radiusMeters) &&
        runtime.planet.radiusMeters > 0.0;
}

struct SurfaceFunctionSample
{
    f64 valueMeters{0.0};
    f64 physicalElevationMeters{0.0};
    f64 renderedElevationMeters{0.0};
    math::Double3 unitDirection{};
};

[[nodiscard]] std::optional<SurfaceFunctionSample>
EvaluateSurfaceFunction(
    const render_view::ViewRay& ray,
    const f64 distanceMeters,
    const render_view::CameraState& camera,
    const u32 viewportHeight,
    const world::PlanetDefinition& planet,
    const terrain::TerrainSource& source) noexcept
{
    if (!std::isfinite(distanceMeters) ||
        distanceMeters < 0.0)
    {
        return std::nullopt;
    }

    const math::Double3 point =
        ray.origin +
        ray.direction *
            distanceMeters;

    const f64 radius =
        math::Length(point);

    if (!std::isfinite(radius) ||
        radius <= 0.0)
    {
        return std::nullopt;
    }

    const math::Double3 direction =
        point / radius;

    const auto terrainSample =
        source.Sample({
            .unitDirection = direction,
            .footprintMeters =
                PixelFootprintMeters(
                    camera,
                    viewportHeight,
                    std::max(
                        distanceMeters,
                        1.0)),
            .planet = planet.id,
            .radialOffsetMeters = 0.0
        });

    if (!std::isfinite(
            terrainSample.elevationMeters) ||
        !std::isfinite(
            terrainSample.standingWaterDepthMeters))
    {
        return std::nullopt;
    }

    const f64 physicalElevation =
        terrainSample.elevationMeters;

    const f64 renderedElevation =
        physicalElevation +
        std::max(
            terrainSample.
                standingWaterDepthMeters,
            0.0);

    return SurfaceFunctionSample{
        .valueMeters =
            radius -
            (planet.radiusMeters +
             renderedElevation),
        .physicalElevationMeters =
            physicalElevation,
        .renderedElevationMeters =
            renderedElevation,
        .unitDirection =
            direction
    };
}

[[nodiscard]] std::optional<f64>
NearIntersectionDistance(
    const render_view::ViewRay& ray,
    const f64 radiusMeters) noexcept
{
    const f64 projection =
        math::Dot(
            ray.origin,
            ray.direction);

    const f64 c =
        math::Dot(
            ray.origin,
            ray.origin) -
        radiusMeters * radiusMeters;

    const f64 discriminant =
        projection * projection -
        c;

    if (!std::isfinite(discriminant) ||
        discriminant < 0.0)
    {
        return std::nullopt;
    }

    const f64 root =
        std::sqrt(
            std::max(
                discriminant,
                0.0));

    const f64 first =
        -projection - root;
    const f64 second =
        -projection + root;

    if (first >= 0.0)
    {
        return first;
    }

    if (second >= 0.0)
    {
        return second;
    }

    return std::nullopt;
}
} // namespace

std::optional<StudioSurfacePick>
PickStudioTerrainSurface(
    const studio_session::ViewportTargetState& viewport,
    const render_view::CameraState& camera,
    const u32 width,
    const u32 height,
    const f32 u,
    const f32 v,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime,
    const terrain::TerrainSource& source,
    const std::optional<u8> physicalTileLevel) noexcept
{
    if (!RuntimeMatchesViewport(
            viewport,
            runtime) ||
        width == 0U ||
        height == 0U ||
        !std::isfinite(u) ||
        !std::isfinite(v) ||
        u < 0.0F ||
        u > 1.0F ||
        v < 0.0F ||
        v > 1.0F)
    {
        return std::nullopt;
    }

    const auto ray =
        render_view::ViewportRay(
            camera,
            width,
            height,
            u,
            v);

    if (!ray.has_value())
    {
        return std::nullopt;
    }

    // Search only the ray's near-body half. The inner envelope is deliberately
    // generous enough for negative terrain while the closest-approach fallback
    // also catches positive relief visible just beyond the reference limb.
    const f64 envelopeMeters =
        std::max(
            runtime.planet.radiusMeters *
                0.02,
            100'000.0);

    const f64 innerRadius =
        std::max(
            runtime.planet.radiusMeters -
                envelopeMeters,
            runtime.planet.radiusMeters *
                0.5);

    const f64 projection =
        math::Dot(
            ray->origin,
            ray->direction);

    const f64 closestDistance =
        -projection;

    if (!std::isfinite(
            closestDistance) ||
        closestDistance <= 0.0)
    {
        return std::nullopt;
    }

    f64 searchEnd =
        closestDistance;

    if (const auto innerHit =
            NearIntersectionDistance(
                *ray,
                innerRadius);
        innerHit.has_value() &&
        *innerHit <= closestDistance)
    {
        searchEnd = *innerHit;
    }

    const auto start =
        EvaluateSurfaceFunction(
            *ray,
            0.0,
            camera,
            height,
            runtime.planet,
            source);

    if (!start.has_value() ||
        start->valueMeters <= 0.0)
    {
        return std::nullopt;
    }

    constexpr u32 kBracketSteps = 96U;

    f64 previousDistance = 0.0;
    SurfaceFunctionSample previous =
        *start;

    std::optional<f64> bracketLow;
    std::optional<f64> bracketHigh;

    for (u32 step = 1U;
         step <= kBracketSteps;
         ++step)
    {
        const f64 fraction =
            static_cast<f64>(step) /
            static_cast<f64>(
                kBracketSteps);

        // Slightly bias samples toward the camera because that is where the
        // first visible intersection normally lies for ground-level editing.
        const f64 distance =
            searchEnd *
            std::sqrt(fraction);

        const auto current =
            EvaluateSurfaceFunction(
                *ray,
                distance,
                camera,
                height,
                runtime.planet,
                source);

        if (!current.has_value())
        {
            return std::nullopt;
        }

        if (previous.valueMeters > 0.0 &&
            current->valueMeters <= 0.0)
        {
            bracketLow =
                previousDistance;
            bracketHigh =
                distance;
            break;
        }

        previousDistance =
            distance;
        previous =
            *current;
    }

    if (!bracketLow.has_value() ||
        !bracketHigh.has_value())
    {
        return std::nullopt;
    }

    // Bisection over the actual sampled terrain radial function gives stable
    // canonical identity without tying the result to a cube face or clipmap.
    for (u32 iteration = 0U;
         iteration < 28U;
         ++iteration)
    {
        const f64 middle =
            (*bracketLow +
             *bracketHigh) *
            0.5;

        const auto sample =
            EvaluateSurfaceFunction(
                *ray,
                middle,
                camera,
                height,
                runtime.planet,
                source);

        if (!sample.has_value())
        {
            return std::nullopt;
        }

        if (sample->valueMeters > 0.0)
        {
            bracketLow = middle;
        }
        else
        {
            bracketHigh = middle;
        }
    }

    const f64 hitDistance =
        *bracketHigh;

    const auto hit =
        EvaluateSurfaceFunction(
            *ray,
            hitDistance,
            camera,
            height,
            runtime.planet,
            source);

    if (!hit.has_value())
    {
        return std::nullopt;
    }

    StudioSurfacePick result{
        .semanticBody =
            runtime.semanticBody,
        .body =
            runtime.body,
        .surface =
            terrain::CanonicalizeSurfacePosition({
                .planet =
                    runtime.planet.id,
                .unitDirection =
                    hit->unitDirection,
                .radialOffsetMeters =
                    hit->
                        physicalElevationMeters
            }),
        .physicalElevationMeters =
            hit->
                physicalElevationMeters,
        .renderedElevationMeters =
            hit->
                renderedElevationMeters,
        .hitDistanceMeters =
            hitDistance,
        .provenance = {
            .viewportId =
                viewport.id,
            .width =
                width,
            .height =
                height,
            .u = u,
            .v = v,
            .ray =
                *ray,
            .universeGeneration =
                runtime.universeGeneration,
            .terrainRuntimeGeneration =
                runtime.runtimeGeneration,
            .terrainSourceRevision =
                runtime.terrainSourceRevision
        }
    };

    const u8 level =
        physicalTileLevel.value_or(
            runtime.physicalPageLevel);

    result.physicalPage =
        terrain::PhysicalTerrainPageAddress{
            .planet =
                runtime.planet.id,
            .tile =
                terrain::PhysicalTileForPosition(
                    result.surface,
                    level)
        };

    result.physicalLod =
        level;

    return result;
}
} // namespace orbit::studio_ui
