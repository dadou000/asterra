#include <orbit/world_model/PrimitiveBinding.hpp>

#include <orbit/world_model/WorldSchemas.hpp>

#include <cmath>
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

[[nodiscard]] bool PositiveFinite(
    const math::Double3& value) noexcept
{
    return std::isfinite(value.x) &&
           std::isfinite(value.y) &&
           std::isfinite(value.z) &&
           value.x > 0.0 &&
           value.y > 0.0 &&
           value.z > 0.0;
}

[[nodiscard]] bool Finite(
    const math::Double3& value) noexcept
{
    return std::isfinite(value.x) &&
           std::isfinite(value.y) &&
           std::isfinite(value.z);
}

void Gather(
    const scene::ObjectStore& objects,
    const scene::ObjectRecord& record,
    std::vector<ResolvedPrimitive>& result)
{
    if (record.type == kPrimitiveType &&
        PropertyOr<bool>(
            objects,
            record.id,
            kPrimitiveEnabled,
            true))
    {
        const i64 shape =
            PropertyOr<i64>(
                objects,
                record.id,
                kPrimitiveShape,
                i64{0});

        const auto size =
            PropertyOr<math::Double3>(
                objects,
                record.id,
                kPrimitiveSizeMeters,
                {1.0, 1.0, 1.0});
        const auto position =
            PropertyOr<math::Double3>(
                objects,
                record.id,
                kPrimitivePositionMeters,
                {});
        const auto euler =
            PropertyOr<math::Double3>(
                objects,
                record.id,
                kPrimitiveEulerDegrees,
                {});

        if (shape >= 0 &&
            shape <= static_cast<i64>(PrimitiveShape::Plane) &&
            PositiveFinite(size) &&
            Finite(position) &&
            Finite(euler))
        {
            result.push_back({
                .object = record.id,
                .shape = static_cast<PrimitiveShape>(shape),
                .positionMeters = position,
                .eulerDegrees = euler,
                .sizeMeters = size,
                .materialAsset =
                    PropertyOr<std::string>(
                        objects,
                        record.id,
                        kPrimitiveMaterialAsset,
                        std::string{}),
                .castShadows =
                    PropertyOr<bool>(
                        objects,
                        record.id,
                        kPrimitiveCastShadows,
                        true)
            });
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

std::vector<ResolvedPrimitive> ResolvePrimitives(
    const scene::ObjectStore& objects,
    const std::optional<scene::ObjectId> root)
{
    std::vector<ResolvedPrimitive> result;

    if (root.has_value())
    {
        if (const auto record = objects.Find(*root);
            record.has_value())
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
