#include <orbit/path_routing/RouteDomains.hpp>

#include <orbit/math/RigidTransform.hpp>
#include <orbit/terrain/TerrainSource.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace orbit::path_routing
{
RouteEnvironment
MakeTerrainSurfaceEnvironment(
    const universe::BodyId body,
    const frames::FrameId routeFrame,
    const time::SimulationTime atTime,
    const frames::FrameGraph& frames,
    const universe::BodyRegistry& bodies,
    const surface::SurfaceRegistry& surfaces,
    const RouteSearchConfig search)
{
    const universe::CelestialBody* bodyRecord =
        bodies.FindBody(body);

    if (bodyRecord == nullptr)
    {
        throw std::invalid_argument(
            "Terrain routing body does not exist.");
    }

    if (!routeFrame ||
        !frames.Contains(routeFrame))
    {
        throw std::invalid_argument(
            "Terrain routing frame does not exist.");
    }

    const auto planet =
        surfaces.SphericalPlanetDefinition(
            body);

    if (!planet.has_value())
    {
        throw std::invalid_argument(
            "Current terrain routing requires a spherical terrain surface.");
    }

    const auto* capability =
        surfaces.FindTerrainSurface(
            body);

    if (capability == nullptr ||
        capability->terrain == nullptr)
    {
        throw std::invalid_argument(
            "Terrain routing requires an attached terrain surface.");
    }

    const auto bodyFromRoute =
        frames.ResolveTransform(
            routeFrame,
            bodyRecord->frame,
            atTime);
    const auto routeFromBody =
        frames.ResolveTransform(
            bodyRecord->frame,
            routeFrame,
            atTime);

    if (!bodyFromRoute.has_value() ||
        !routeFromBody.has_value())
    {
        throw std::invalid_argument(
            "Terrain routing frame is not connected to the body frame.");
    }

    const auto terrain =
        capability->terrain;
    const f64 radius =
        planet->radiusMeters;
    const math::RigidTransformD
        capturedBodyFromRoute =
            *bodyFromRoute;
    const math::RigidTransformD
        capturedRouteFromBody =
            *routeFromBody;

    RouteEnvironment environment{
        .frame = routeFrame,
        .domainKey =
            "terrain:" +
            body.ToString(),
        .domainRevision =
            [terrain]
            {
                return terrain->Revision();
            },
        .projector =
            [terrain,
             radius,
             capturedBodyFromRoute,
             capturedRouteFromBody](
                const math::Double3& candidate,
                const f64 footprintMeters)
                -> std::optional<
                    RouteProjectedPoint>
            {
                const math::Double3
                    bodyPoint =
                        math::TransformPoint(
                            capturedBodyFromRoute,
                            candidate);

                const math::Double3 direction =
                    math::Normalize(
                        bodyPoint);

                if (math::LengthSquared(
                        direction) <=
                    1.0e-20)
                {
                    return std::nullopt;
                }

                const terrain::TerrainSample sample =
                    terrain->Sample({
                        .unitDirection =
                            direction,
                        .footprintMeters =
                            std::max(
                                footprintMeters,
                                0.01)
                    });

                if (!std::isfinite(
                        sample.elevationMeters) ||
                    !std::isfinite(
                        sample.
                            standingWaterDepthMeters))
                {
                    return std::nullopt;
                }

                const math::Double3
                    terrainPoint =
                        direction *
                        (radius +
                         sample.
                             elevationMeters);

                return RouteProjectedPoint{
                    .localMeters =
                        math::TransformPoint(
                            capturedRouteFromBody,
                            terrainPoint),
                    .elevationMeters =
                        sample.
                            elevationMeters,
                    .waterDepthMeters =
                        std::max(
                            sample.
                                standingWaterDepthMeters,
                            0.0)
                };
            },
        .search = search
    };

    return environment;
}

RouteCostSource
MakeSurfaceScalarFieldCostSource(
    std::string key,
    const fields::FieldId field,
    const f64 weight,
    const universe::BodyId body,
    const frames::FrameId routeFrame,
    const time::SimulationTime atTime,
    const frames::FrameGraph& frames,
    const universe::BodyRegistry& bodies,
    const fields::FieldRegistry& fields)
{
    if (key.empty())
    {
        throw std::invalid_argument(
            "Route field cost source requires a stable key.");
    }

    if (!std::isfinite(weight) ||
        weight < 0.0)
    {
        throw std::invalid_argument(
            "Route field cost weight must be finite and non-negative.");
    }

    const fields::FieldDescriptor* descriptor =
        fields.Find(field);

    if (descriptor == nullptr)
    {
        throw std::invalid_argument(
            "Route field cost references an unknown field.");
    }

    if (descriptor->valueKind !=
        fields::FieldValueKind::Scalar)
    {
        throw std::invalid_argument(
            "Route field cost requires a scalar field.");
    }

    if (descriptor->domain !=
        fields::FieldDomain::Surface)
    {
        throw std::invalid_argument(
            "Route field cost requires a surface-domain field.");
    }

    if (descriptor->ownerBody.has_value() &&
        *descriptor->ownerBody != body)
    {
        throw std::invalid_argument(
            "Route field cost belongs to a different body.");
    }

    const universe::CelestialBody* bodyRecord =
        bodies.FindBody(body);

    if (bodyRecord == nullptr)
    {
        throw std::invalid_argument(
            "Route field cost body does not exist.");
    }

    if (!routeFrame ||
        !frames.Contains(routeFrame))
    {
        throw std::invalid_argument(
            "Route field cost frame does not exist.");
    }

    const auto bodyFromRoute =
        frames.ResolveTransform(
            routeFrame,
            bodyRecord->frame,
            atTime);

    if (!bodyFromRoute.has_value())
    {
        throw std::invalid_argument(
            "Route field cost frame is not connected to the body frame.");
    }

    const math::RigidTransformD
        capturedBodyFromRoute =
            *bodyFromRoute;

    const fields::FieldRegistry* registry =
        &fields;

    return RouteCostSource{
        .key = std::move(key),
        .revision =
            [registry, field]
            {
                return registry->Revision(
                    field);
            },
        .evaluateCostPerMeter =
            [registry,
             field,
             body,
             weight,
             capturedBodyFromRoute](
                const RouteProjectedPoint& point,
                const f64 footprintMeters)
                -> std::optional<f64>
            {
                const math::Double3 bodyPoint =
                    math::TransformPoint(
                        capturedBodyFromRoute,
                        point.localMeters);

                const math::Double3 direction =
                    math::Normalize(
                        bodyPoint);

                if (math::LengthSquared(
                        direction) <=
                    1.0e-20)
                {
                    return std::nullopt;
                }

                const auto sampled =
                    registry->TrySampleCpu(
                        field,
                        fields::SurfaceFieldLocation{
                            .body = body,
                            .unitDirection =
                                direction,
                            .footprintMeters =
                                std::max(
                                    footprintMeters,
                                    0.01)
                        });

                if (!sampled.has_value())
                {
                    return std::nullopt;
                }

                const auto* scalar =
                    std::get_if<f64>(
                        &*sampled);

                if (scalar == nullptr ||
                    !std::isfinite(*scalar))
                {
                    return std::nullopt;
                }

                return
                    std::max(
                        *scalar,
                        0.0) *
                    weight;
            }
    };
}

std::vector<RouteCostSource>
MakePreferredSurfaceFieldCosts(
    const paths::PathProfile& profile,
    const f64 weight,
    const universe::BodyId body,
    const frames::FrameId routeFrame,
    const time::SimulationTime atTime,
    const frames::FrameGraph& frames,
    const universe::BodyRegistry& bodies,
    const fields::FieldRegistry& fields)
{
    std::vector<RouteCostSource> result;

    for (const std::string& requested :
         profile.preferredCostFields)
    {
        const auto bodyFields =
            fields.FieldsForBody(
                body);

        const auto found =
            std::ranges::find_if(
                bodyFields,
                [&](const fields::FieldId id)
                {
                    const auto* descriptor =
                        fields.Find(id);

                    return descriptor != nullptr &&
                        descriptor->name ==
                            requested &&
                        descriptor->valueKind ==
                            fields::FieldValueKind::
                                Scalar &&
                        descriptor->domain ==
                            fields::FieldDomain::
                                Surface &&
                        descriptor->residency !=
                            fields::FieldResidency::
                                Gpu;
                });

        if (found ==
            bodyFields.end())
        {
            continue;
        }

        result.push_back(
            MakeSurfaceScalarFieldCostSource(
                "field:" +
                    found->ToString(),
                *found,
                weight,
                body,
                routeFrame,
                atTime,
                frames,
                bodies,
                fields));
    }

    return result;
}
} // namespace orbit::path_routing
