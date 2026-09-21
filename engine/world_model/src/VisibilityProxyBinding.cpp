#include <orbit/world_model/VisibilityProxyBinding.hpp>

#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <variant>

namespace orbit::world_model
{
namespace
{
template <typename T>
[[nodiscard]] T PropertyOr(
    const scene::ObjectStore& objects,
    const scene::ObjectId object,
    const schema::PropertyId property,
    const T& fallback)
{
    const auto value =
        objects.GetProperty(
            object,
            property);

    if (!value.has_value())
    {
        return fallback;
    }

    if (const auto* typed =
            std::get_if<T>(&*value))
    {
        return *typed;
    }

    return fallback;
}

[[nodiscard]] u32 NonNegativeU32(
    const i64 value) noexcept
{
    if (value <= 0)
    {
        return 0U;
    }

    return static_cast<u32>(
        std::min<i64>(
            value,
            std::numeric_limits<u32>::max()));
}

void Gather(
    const scene::ObjectStore& objects,
    const scene::ObjectRecord& record,
    std::vector<ResolvedVisibilityProxy>& result)
{
    if (record.type ==
        kVisibilityProxyType)
    {
        const bool enabled =
            PropertyOr<bool>(
                objects,
                record.id,
                kVisibilityProxyEnabled,
                true);

        if (enabled)
        {
            const i64 shapeValue =
                PropertyOr<i64>(
                    objects,
                    record.id,
                    kVisibilityProxyShape,
                    i64{0});

            const f64 radius =
                PropertyOr<f64>(
                    objects,
                    record.id,
                    kVisibilityProxyRadiusMeters,
                    0.5);

            const auto extents =
                PropertyOr<math::Double3>(
                    objects,
                    record.id,
                    kVisibilityProxyHalfExtentsMeters,
                    {0.5, 0.5, 0.5});

            const f64 error =
                PropertyOr<f64>(
                    objects,
                    record.id,
                    kVisibilityProxyErrorMeters,
                    0.25);

            const bool finiteExtents =
                std::isfinite(extents.x) &&
                std::isfinite(extents.y) &&
                std::isfinite(extents.z);

            if (std::isfinite(radius) &&
                radius > 0.0 &&
                finiteExtents &&
                extents.x > 0.0 &&
                extents.y > 0.0 &&
                extents.z > 0.0 &&
                std::isfinite(error) &&
                error >= 0.0)
            {
                result.push_back({
                    .object = record.id,
                    .shape =
                        shapeValue == 1
                            ? ResolvedVisibilityProxyShape::Box
                            : ResolvedVisibilityProxyShape::Sphere,
                    .positionMeters =
                        PropertyOr<math::Double3>(
                            objects,
                            record.id,
                            kVisibilityProxyPositionMeters,
                            {}),
                    .eulerDegrees =
                        PropertyOr<math::Double3>(
                            objects,
                            record.id,
                            kVisibilityProxyEulerDegrees,
                            {}),
                    .radiusMeters = radius,
                    .halfExtentsMeters =
                        extents,
                    .materialId =
                        NonNegativeU32(
                            PropertyOr<i64>(
                                objects,
                                record.id,
                                kVisibilityProxyMaterialId,
                                i64{0})),
                    .instanceId =
                        NonNegativeU32(
                            PropertyOr<i64>(
                                objects,
                                record.id,
                                kVisibilityProxyInstanceId,
                                i64{0})),
                    .maximumApproximationErrorMeters =
                        static_cast<f32>(error),
                    .dynamic =
                        PropertyOr<bool>(
                            objects,
                            record.id,
                            kVisibilityProxyDynamic,
                            false)
                });
            }
        }
    }

    for (const auto& child :
         objects.Children(record.id))
    {
        Gather(
            objects,
            child,
            result);
    }
}
} // namespace

std::vector<ResolvedVisibilityProxy>
ResolveVisibilityProxies(
    const scene::ObjectStore& objects,
    const std::optional<scene::ObjectId> root)
{
    std::vector<ResolvedVisibilityProxy> result;

    if (root.has_value())
    {
        const auto record =
            objects.Find(*root);

        if (record.has_value())
        {
            Gather(
                objects,
                *record,
                result);
        }

        return result;
    }

    for (const auto& record :
         objects.Roots())
    {
        Gather(
            objects,
            record,
            result);
    }

    return result;
}
} // namespace orbit::world_model
