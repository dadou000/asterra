#include <orbit/surface_authoring/TerrainConstraints.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace orbit::surface_authoring
{
namespace
{
constexpr f64 Pi = 3.1415926535897932384626433832795;
constexpr f64 Epsilon = 1.0e-9;

[[nodiscard]] bool Finite(const f64 value) noexcept
{
    return std::isfinite(value);
}

[[nodiscard]] bool UnitDirectionValid(
    const math::Double3& direction) noexcept
{
    return
        Finite(direction.x) &&
        Finite(direction.y) &&
        Finite(direction.z) &&
        math::LengthSquared(direction) > Epsilon;
}

[[nodiscard]] math::Double3 Unit(
    const math::Double3& direction) noexcept
{
    if (!UnitDirectionValid(direction))
    {
        return {0.0, 1.0, 0.0};
    }

    return math::Normalize(direction);
}

[[nodiscard]] f64 ClampUnit(const f64 value) noexcept
{
    return std::clamp(value, 0.0, 1.0);
}

[[nodiscard]] f64 AngularDistance(
    const math::Double3& left,
    const math::Double3& right) noexcept
{
    return std::acos(
        std::clamp(
            math::Dot(Unit(left), Unit(right)),
            -1.0,
            1.0));
}

[[nodiscard]] f64 SurfaceDistance(
    const world::PlanetDefinition& planet,
    const math::Double3& left,
    const math::Double3& right) noexcept
{
    return AngularDistance(left, right) *
        planet.radiusMeters;
}

[[nodiscard]] f64 SmoothFalloff(
    const f64 distance,
    const f64 inner,
    const f64 outer) noexcept
{
    if (distance <= inner)
    {
        return 1.0;
    }

    if (outer <= inner || distance >= outer)
    {
        return 0.0;
    }

    const f64 t =
        ClampUnit((distance - inner) / (outer - inner));
    const f64 smooth = t * t * (3.0 - 2.0 * t);
    return 1.0 - smooth;
}

[[nodiscard]] f64 GreatCircleSegmentDistance(
    const world::PlanetDefinition& planet,
    const math::Double3& startValue,
    const math::Double3& endValue,
    const math::Double3& queryValue) noexcept
{
    const math::Double3 start = Unit(startValue);
    const math::Double3 end = Unit(endValue);
    const math::Double3 query = Unit(queryValue);

    const f64 segmentAngle =
        AngularDistance(start, end);

    if (segmentAngle <= 1.0e-10 ||
        segmentAngle >= Pi - 1.0e-8)
    {
        return std::min(
            SurfaceDistance(planet, start, query),
            SurfaceDistance(planet, end, query));
    }

    const math::Double3 normalRaw =
        math::Cross(start, end);
    const f64 normalLength =
        math::Length(normalRaw);

    if (normalLength <= Epsilon)
    {
        return std::min(
            SurfaceDistance(planet, start, query),
            SurfaceDistance(planet, end, query));
    }

    const math::Double3 normal =
        normalRaw / normalLength;
    const math::Double3 projectedRaw =
        query - normal * math::Dot(query, normal);

    if (math::LengthSquared(projectedRaw) <= Epsilon)
    {
        return std::min(
            SurfaceDistance(planet, start, query),
            SurfaceDistance(planet, end, query));
    }

    math::Double3 projected = Unit(projectedRaw);

    auto onMinorArc =
        [&](const math::Double3& candidate)
        {
            const f64 split =
                AngularDistance(start, candidate) +
                AngularDistance(candidate, end);
            return std::abs(split - segmentAngle) <= 1.0e-6;
        };

    if (!onMinorArc(projected))
    {
        projected = projected * -1.0;
    }

    if (onMinorArc(projected))
    {
        return SurfaceDistance(
            planet,
            query,
            projected);
    }

    return std::min(
        SurfaceDistance(planet, start, query),
        SurfaceDistance(planet, end, query));
}

[[nodiscard]] f64 DistanceToSegment2D(
    const math::Double2& point,
    const math::Double2& a,
    const math::Double2& b) noexcept
{
    const math::Double2 delta = b - a;
    const f64 denominator =
        delta.x * delta.x +
        delta.y * delta.y;

    if (denominator <= Epsilon)
    {
        const math::Double2 offset = point - a;
        return std::sqrt(
            offset.x * offset.x +
            offset.y * offset.y);
    }

    const math::Double2 offset = point - a;
    const f64 t = std::clamp(
        (offset.x * delta.x +
         offset.y * delta.y) /
            denominator,
        0.0,
        1.0);
    const math::Double2 closest{
        a.x + delta.x * t,
        a.y + delta.y * t
    };
    const math::Double2 difference = point - closest;
    return std::sqrt(
        difference.x * difference.x +
        difference.y * difference.y);
}

[[nodiscard]] math::Double3 PolygonAnchor(
    const PolygonConstraintPrimitive& polygon) noexcept
{
    math::Double3 sum{};
    for (const math::Double3& vertex :
         polygon.verticesUnitDirections)
    {
        sum = sum + Unit(vertex);
    }

    if (math::LengthSquared(sum) <= Epsilon)
    {
        return Unit(
            polygon.verticesUnitDirections.front());
    }

    return Unit(sum);
}

[[nodiscard]] bool PointInsidePolygon(
    const math::Double2& point,
    const std::vector<math::Double2>& vertices) noexcept
{
    bool inside = false;

    for (std::size_t i = 0,
                     j = vertices.size() - 1;
         i < vertices.size();
         j = i++)
    {
        const auto& vi = vertices[i];
        const auto& vj = vertices[j];
        const bool crosses =
            ((vi.y > point.y) !=
             (vj.y > point.y)) &&
            (point.x <
             (vj.x - vi.x) *
                     (point.y - vi.y) /
                     ((vj.y - vi.y) +
                      std::numeric_limits<f64>::epsilon()) +
                 vi.x);

        if (crosses)
        {
            inside = !inside;
        }
    }

    return inside;
}

[[nodiscard]] f64 EvaluatePolygon(
    const PolygonConstraintPrimitive& polygon,
    const world::PlanetDefinition& planet,
    const terrain::PlanetSurfacePosition& position) noexcept
{
    const math::Double3 anchorDirection =
        PolygonAnchor(polygon);
    terrain::PlanetSurfacePosition anchor{
        .planet = position.planet,
        .unitDirection = anchorDirection,
        .radialOffsetMeters = 0.0
    };
    const world::SurfaceFrame frame =
        terrain::SurfaceTangentFrame(anchor);

    std::vector<math::Double2> vertices;
    vertices.reserve(
        polygon.verticesUnitDirections.size());

    for (const auto& direction :
         polygon.verticesUnitDirections)
    {
        terrain::PlanetSurfacePosition vertex{
            .planet = position.planet,
            .unitDirection = direction,
            .radialOffsetMeters = 0.0
        };
        vertices.push_back(
            terrain::SurfaceOffsetBetweenPositions(
                planet,
                anchor,
                frame,
                vertex));
    }

    const math::Double2 query =
        terrain::SurfaceOffsetBetweenPositions(
            planet,
            anchor,
            frame,
            position);

    if (PointInsidePolygon(query, vertices))
    {
        return 1.0;
    }

    if (polygon.falloffMeters <= 0.0)
    {
        return 0.0;
    }

    f64 distance =
        std::numeric_limits<f64>::infinity();

    for (std::size_t index = 0;
         index < vertices.size();
         ++index)
    {
        const std::size_t next =
            (index + 1) % vertices.size();
        distance = std::min(
            distance,
            DistanceToSegment2D(
                query,
                vertices[index],
                vertices[next]));
    }

    return SmoothFalloff(
        distance,
        0.0,
        polygon.falloffMeters);
}

[[nodiscard]] f64 EvaluateRaster(
    const RasterMaskConstraintPrimitive& raster,
    const world::PlanetDefinition& planet,
    const terrain::PlanetSurfacePosition& position) noexcept
{
    terrain::PlanetSurfacePosition anchor{
        .planet = position.planet,
        .unitDirection = raster.anchorUnitDirection,
        .radialOffsetMeters = 0.0
    };
    const math::Double2 offset =
        terrain::SurfaceOffsetBetweenPositions(
            planet,
            anchor,
            position);

    const f64 cosine =
        std::cos(-raster.rotationRadians);
    const f64 sine =
        std::sin(-raster.rotationRadians);
    const math::Double2 local{
        offset.x * cosine - offset.y * sine,
        offset.x * sine + offset.y * cosine
    };

    const f64 centerX =
        0.5 * static_cast<f64>(raster.width - 1);
    const f64 centerY =
        0.5 * static_cast<f64>(raster.height - 1);
    const f64 x =
        local.x / raster.cellSizeMeters + centerX;
    const f64 y =
        local.y / raster.cellSizeMeters + centerY;

    if (x < 0.0 || y < 0.0 ||
        x > static_cast<f64>(raster.width - 1) ||
        y > static_cast<f64>(raster.height - 1))
    {
        return 0.0;
    }

    const u32 x0 = static_cast<u32>(std::floor(x));
    const u32 y0 = static_cast<u32>(std::floor(y));
    const u32 x1 = std::min(x0 + 1, raster.width - 1);
    const u32 y1 = std::min(y0 + 1, raster.height - 1);
    const f64 tx = x - static_cast<f64>(x0);
    const f64 ty = y - static_cast<f64>(y0);

    auto sample =
        [&](const u32 sx, const u32 sy)
        {
            return static_cast<f64>(
                raster.samples[
                    static_cast<std::size_t>(sy) * raster.width + sx]);
        };

    const f64 top =
        sample(x0, y0) * (1.0 - tx) +
        sample(x1, y0) * tx;
    const f64 bottom =
        sample(x0, y1) * (1.0 - tx) +
        sample(x1, y1) * tx;

    return ClampUnit(
        top * (1.0 - ty) +
        bottom * ty);
}

[[nodiscard]] bool PrimitiveValid(
    const TerrainConstraintPrimitive& primitive) noexcept
{
    return std::visit(
        [](const auto& value)
        {
            return value.IsValid();
        },
        primitive);
}

[[nodiscard]] f64 ApplyScalar(
    const f64 current,
    const f64 authored,
    const ConstraintCompositionMode mode,
    const f64 influence) noexcept
{
    const f64 t = ClampUnit(influence);

    switch (mode)
    {
    case ConstraintCompositionMode::Add:
        return current + authored * t;
    case ConstraintCompositionMode::Subtract:
        return current - authored * t;
    case ConstraintCompositionMode::Replace:
        return current + (authored - current) * t;
    case ConstraintCompositionMode::Multiply:
        return current * (1.0 + (authored - 1.0) * t);
    case ConstraintCompositionMode::Min:
        return current +
            (std::min(current, authored) - current) * t;
    case ConstraintCompositionMode::Max:
        return current +
            (std::max(current, authored) - current) * t;
    }

    return current;
}

void AddMaterialWeight(
    MaterialConstraintSample& sample,
    const terrain_geology::RockTypeId material,
    const f64 delta)
{
    if (!material.IsValid() ||
        std::abs(delta) <= 1.0e-12)
    {
        return;
    }

    for (u32 index = 0;
         index < sample.count;
         ++index)
    {
        if (sample.materials[index] == material)
        {
            sample.weights[index] =
                static_cast<f32>(
                    std::max(
                        0.0,
                        static_cast<f64>(sample.weights[index]) +
                            delta));
            return;
        }
    }

    if (delta <= 0.0)
    {
        return;
    }

    if (sample.count <
        MaterialConstraintSample::MaxMaterials)
    {
        const u32 index = sample.count++;
        sample.materials[index] = material;
        sample.weights[index] =
            static_cast<f32>(delta);
        return;
    }

    u32 weakest = 0;
    for (u32 index = 1;
         index < sample.count;
         ++index)
    {
        if (sample.weights[index] <
            sample.weights[weakest])
        {
            weakest = index;
        }
    }

    if (delta > sample.weights[weakest])
    {
        sample.materials[weakest] = material;
        sample.weights[weakest] =
            static_cast<f32>(delta);
    }
}

[[nodiscard]] f64 MaterialWeight(
    const MaterialConstraintSample& sample,
    const terrain_geology::RockTypeId material) noexcept
{
    for (u32 index = 0;
         index < sample.count;
         ++index)
    {
        if (sample.materials[index] == material)
        {
            return sample.weights[index];
        }
    }

    return 0.0;
}

void SetMaterialWeight(
    MaterialConstraintSample& sample,
    const terrain_geology::RockTypeId material,
    const f64 value)
{
    const f64 current =
        MaterialWeight(sample, material);
    AddMaterialWeight(
        sample,
        material,
        value - current);
}

void ApplyMaterial(
    MaterialConstraintSample& sample,
    const MaterialTerrainConstraint& constraint,
    const f64 influence)
{
    const f64 t = ClampUnit(influence);
    if (t <= 0.0)
    {
        return;
    }

    const f64 authored =
        ClampUnit(constraint.weight);
    const f64 current =
        MaterialWeight(sample, constraint.material);

    switch (constraint.mode)
    {
    case ConstraintCompositionMode::Add:
        AddMaterialWeight(
            sample,
            constraint.material,
            authored * t);
        break;
    case ConstraintCompositionMode::Subtract:
        AddMaterialWeight(
            sample,
            constraint.material,
            -authored * t);
        break;
    case ConstraintCompositionMode::Replace:
        for (u32 index = 0;
             index < sample.count;
             ++index)
        {
            sample.weights[index] =
                static_cast<f32>(
                    static_cast<f64>(sample.weights[index]) *
                    (1.0 - t));
        }
        AddMaterialWeight(
            sample,
            constraint.material,
            authored * t);
        break;
    case ConstraintCompositionMode::Multiply:
        SetMaterialWeight(
            sample,
            constraint.material,
            current *
                (1.0 + (authored - 1.0) * t));
        break;
    case ConstraintCompositionMode::Min:
        SetMaterialWeight(
            sample,
            constraint.material,
            current +
                (std::min(current, authored) - current) * t);
        break;
    case ConstraintCompositionMode::Max:
        SetMaterialWeight(
            sample,
            constraint.material,
            current +
                (std::max(current, authored) - current) * t);
        break;
    }
}

void NormalizeMaterialWeights(
    MaterialConstraintSample& sample) noexcept
{
    f64 sum = 0.0;
    for (u32 index = 0;
         index < sample.count;
         ++index)
    {
        sum += std::max(
            0.0,
            static_cast<f64>(sample.weights[index]));
    }

    if (sum <= 1.0e-12)
    {
        sample = {};
        return;
    }

    for (u32 index = 0;
         index < sample.count;
         ++index)
    {
        sample.weights[index] =
            static_cast<f32>(
                static_cast<f64>(sample.weights[index]) /
                sum);
    }

    for (u32 left = 0;
         left < sample.count;
         ++left)
    {
        for (u32 right = left + 1;
             right < sample.count;
             ++right)
        {
            if (sample.weights[right] >
                sample.weights[left])
            {
                std::swap(
                    sample.weights[left],
                    sample.weights[right]);
                std::swap(
                    sample.materials[left],
                    sample.materials[right]);
            }
        }
    }
}

template <typename Constraint>
[[nodiscard]] bool ConstraintsValid(
    const std::vector<Constraint>& constraints,
    std::unordered_set<TerrainConstraintId>& ids) noexcept
{
    for (const Constraint& constraint : constraints)
    {
        if (!constraint.IsValid() ||
            !ids.insert(constraint.id).second)
        {
            return false;
        }
    }

    return true;
}
} // namespace

bool PointConstraintPrimitive::IsValid() const noexcept
{
    return
        UnitDirectionValid(centerUnitDirection) &&
        Finite(radiusMeters) &&
        radiusMeters > 0.0;
}

bool BrushConstraintPrimitive::IsValid() const noexcept
{
    return
        UnitDirectionValid(centerUnitDirection) &&
        Finite(innerRadiusMeters) &&
        Finite(outerRadiusMeters) &&
        innerRadiusMeters >= 0.0 &&
        outerRadiusMeters > innerRadiusMeters;
}

bool SplineConstraintPrimitive::IsValid() const noexcept
{
    if (controlUnitDirections.size() < 2 ||
        !Finite(halfWidthMeters) ||
        !Finite(falloffMeters) ||
        halfWidthMeters < 0.0 ||
        falloffMeters < 0.0)
    {
        return false;
    }

    for (std::size_t index = 0;
         index < controlUnitDirections.size();
         ++index)
    {
        if (!UnitDirectionValid(
                controlUnitDirections[index]))
        {
            return false;
        }

        if (index > 0 &&
            AngularDistance(
                controlUnitDirections[index - 1],
                controlUnitDirections[index]) >=
                Pi - 1.0e-6)
        {
            return false;
        }
    }

    return true;
}

bool PolygonConstraintPrimitive::IsValid() const noexcept
{
    if (verticesUnitDirections.size() < 3 ||
        !Finite(falloffMeters) ||
        falloffMeters < 0.0)
    {
        return false;
    }

    return std::ranges::all_of(
        verticesUnitDirections,
        [](const math::Double3& value)
        {
            return UnitDirectionValid(value);
        });
}

bool RasterMaskConstraintPrimitive::IsValid() const noexcept
{
    if (!UnitDirectionValid(anchorUnitDirection) ||
        !Finite(rotationRadians) ||
        !Finite(cellSizeMeters) ||
        cellSizeMeters <= 0.0 ||
        width == 0 ||
        height == 0 ||
        samples.size() !=
            static_cast<std::size_t>(width) * height)
    {
        return false;
    }

    return std::ranges::all_of(
        samples,
        [](const f32 value)
        {
            return
                std::isfinite(value) &&
                value >= 0.0F &&
                value <= 1.0F;
        });
}

bool ScalarTerrainConstraint::IsValid() const noexcept
{
    return
        id.IsValid() &&
        PrimitiveValid(primitive) &&
        Finite(value) &&
        Finite(opacity) &&
        opacity >= 0.0 &&
        opacity <= 1.0;
}

bool GradientTerrainConstraint::IsValid() const noexcept
{
    return
        id.IsValid() &&
        PrimitiveValid(primitive) &&
        Finite(value.x) &&
        Finite(value.y) &&
        Finite(opacity) &&
        opacity >= 0.0 &&
        opacity <= 1.0;
}

bool MaterialTerrainConstraint::IsValid() const noexcept
{
    return
        id.IsValid() &&
        PrimitiveValid(primitive) &&
        material.IsValid() &&
        Finite(weight) &&
        weight >= 0.0 &&
        weight <= 1.0 &&
        Finite(opacity) &&
        opacity >= 0.0 &&
        opacity <= 1.0;
}

bool TerrainConstraintSet::IsValid() const noexcept
{
    if (!id.IsValid() ||
        !planet.IsValid() ||
        name.empty())
    {
        return false;
    }

    std::unordered_set<TerrainConstraintId> ids;
    return
        ConstraintsValid(height.constraints, ids) &&
        ConstraintsValid(gradient.constraints, ids) &&
        ConstraintsValid(uplift.constraints, ids) &&
        ConstraintsValid(material.constraints, ids) &&
        ConstraintsValid(protection.constraints, ids) &&
        ConstraintsValid(drainage.constraints, ids);
}

f64 EvaluateConstraintInfluence(
    const TerrainConstraintPrimitive& primitive,
    const world::PlanetDefinition& planet,
    const terrain::PlanetSurfacePosition& position) noexcept
{
    if (!position.IsValid() ||
        planet.radiusMeters <= 0.0)
    {
        return 0.0;
    }

    return std::visit(
        [&](const auto& value) -> f64
        {
            using T = std::decay_t<decltype(value)>;

            if constexpr (std::is_same_v<T,
                          PointConstraintPrimitive>)
            {
                const f64 distance =
                    SurfaceDistance(
                        planet,
                        value.centerUnitDirection,
                        position.unitDirection);
                return SmoothFalloff(
                    distance,
                    0.0,
                    value.radiusMeters);
            }
            else if constexpr (std::is_same_v<T,
                               BrushConstraintPrimitive>)
            {
                const f64 distance =
                    SurfaceDistance(
                        planet,
                        value.centerUnitDirection,
                        position.unitDirection);
                return SmoothFalloff(
                    distance,
                    value.innerRadiusMeters,
                    value.outerRadiusMeters);
            }
            else if constexpr (std::is_same_v<T,
                               SplineConstraintPrimitive>)
            {
                f64 distance =
                    std::numeric_limits<f64>::infinity();
                for (std::size_t index = 1;
                     index < value.controlUnitDirections.size();
                     ++index)
                {
                    distance = std::min(
                        distance,
                        GreatCircleSegmentDistance(
                            planet,
                            value.controlUnitDirections[index - 1],
                            value.controlUnitDirections[index],
                            position.unitDirection));
                }

                return SmoothFalloff(
                    distance,
                    value.halfWidthMeters,
                    value.halfWidthMeters +
                        value.falloffMeters);
            }
            else if constexpr (std::is_same_v<T,
                               PolygonConstraintPrimitive>)
            {
                return EvaluatePolygon(
                    value,
                    planet,
                    position);
            }
            else
            {
                return EvaluateRaster(
                    value,
                    planet,
                    position);
            }
        },
        primitive);
}

TerrainConstraintSample EvaluateTerrainConstraintSet(
    const TerrainConstraintSet& constraints,
    const world::PlanetDefinition& planet,
    const terrain::PlanetSurfacePosition& position,
    const TerrainConstraintBaseline& baseline)
{
    if (!constraints.IsValid())
    {
        throw std::invalid_argument(
            "Cannot evaluate an invalid terrain constraint set.");
    }

    const terrain::PlanetSurfacePosition canonical =
        terrain::CanonicalizeSurfacePosition(position);

    if (constraints.planet != planet.id ||
        canonical.planet != constraints.planet)
    {
        throw std::invalid_argument(
            "Terrain constraint set, planet definition and query position must share one PlanetId.");
    }

    TerrainConstraintSample result{
        .heightMeters = baseline.heightMeters,
        .gradient = baseline.gradient,
        .upliftMeters = baseline.upliftMeters,
        .material = {},
        .protection = baseline.protection,
        .drainage = baseline.drainage
    };

    if (baseline.material.IsValid())
    {
        result.material.materials[0] =
            baseline.material;
        result.material.weights[0] = 1.0F;
        result.material.count = 1;
    }

    auto influence =
        [&](const auto& constraint)
        {
            if (!constraint.enabled)
            {
                return 0.0;
            }

            return EvaluateConstraintInfluence(
                       constraint.primitive,
                       planet,
                       canonical) *
                constraint.opacity;
        };

    for (const auto& constraint :
         constraints.height.constraints)
    {
        result.heightMeters = ApplyScalar(
            result.heightMeters,
            constraint.value,
            constraint.mode,
            influence(constraint));
    }

    for (const auto& constraint :
         constraints.gradient.constraints)
    {
        const f64 t = influence(constraint);
        result.gradient.x = ApplyScalar(
            result.gradient.x,
            constraint.value.x,
            constraint.mode,
            t);
        result.gradient.y = ApplyScalar(
            result.gradient.y,
            constraint.value.y,
            constraint.mode,
            t);
    }

    for (const auto& constraint :
         constraints.uplift.constraints)
    {
        result.upliftMeters = ApplyScalar(
            result.upliftMeters,
            constraint.value,
            constraint.mode,
            influence(constraint));
    }

    for (const auto& constraint :
         constraints.material.constraints)
    {
        if (constraint.enabled)
        {
            ApplyMaterial(
                result.material,
                constraint,
                influence(constraint));
        }
    }

    for (const auto& constraint :
         constraints.protection.constraints)
    {
        result.protection = ApplyScalar(
            result.protection,
            constraint.value,
            constraint.mode,
            influence(constraint));
    }

    for (const auto& constraint :
         constraints.drainage.constraints)
    {
        result.drainage = ApplyScalar(
            result.drainage,
            constraint.value,
            constraint.mode,
            influence(constraint));
    }

    result.protection =
        ClampUnit(result.protection);
    NormalizeMaterialWeights(
        result.material);
    return result;
}

f64 ErosionAllowanceFromProtection(
    const f64 protection) noexcept
{
    return 1.0 - ClampUnit(protection);
}

bool ReferencesKnownMaterials(
    const TerrainConstraintSet& constraints,
    const terrain_geology::GeologicalMaterialLibrary& materials) noexcept
{
    for (const auto& constraint :
         constraints.material.constraints)
    {
        if (materials.Find(constraint.material) ==
            nullptr)
        {
            return false;
        }
    }

    return true;
}
} // namespace orbit::surface_authoring
