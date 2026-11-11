#include <orbit/world_model/StaticMeshBinding.hpp>

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
    const auto value = objects.GetProperty(object, property);

    if (!value.has_value())
    {
        return fallback;
    }

    if (const auto* typed = std::get_if<T>(&*value))
    {
        return *typed;
    }

    return fallback;
}

[[nodiscard]] bool Finite(const math::Double3& value) noexcept
{
    return std::isfinite(value.x) &&
           std::isfinite(value.y) &&
           std::isfinite(value.z);
}

void Gather(
    const scene::ObjectStore& objects,
    const scene::ObjectRecord& record,
    std::vector<ResolvedStaticMesh>& result)
{
    if (record.type == kStaticMeshType &&
        PropertyOr<bool>(objects, record.id, kStaticMeshEnabled, true))
    {
        auto asset =
            PropertyOr<std::string>(
                objects, record.id, kStaticMeshAsset, std::string{});
        const auto position =
            PropertyOr<math::Double3>(
                objects, record.id, kStaticMeshPositionMeters, {});
        const auto euler =
            PropertyOr<math::Double3>(
                objects, record.id, kStaticMeshEulerDegrees, {});
        const f64 scale =
            PropertyOr<f64>(objects, record.id, kStaticMeshScale, 1.0);

        if (!asset.empty() &&
            Finite(position) &&
            Finite(euler) &&
            std::isfinite(scale) &&
            scale > 0.0)
        {
            result.push_back({
                .object = record.id,
                .meshAsset = std::move(asset),
                .positionMeters = position,
                .eulerDegrees = euler,
                .uniformScale = scale,
                .castShadows =
                    PropertyOr<bool>(
                        objects, record.id, kStaticMeshCastShadows, true)
            });
        }
    }

    for (const auto& child : objects.Children(record.id))
    {
        Gather(objects, child, result);
    }
}
} // namespace

std::vector<ResolvedStaticMesh> ResolveStaticMeshes(
    const scene::ObjectStore& objects,
    const std::optional<scene::ObjectId> root)
{
    std::vector<ResolvedStaticMesh> result;

    if (root.has_value())
    {
        if (const auto record = objects.Find(*root); record.has_value())
        {
            Gather(objects, *record, result);
        }

        return result;
    }

    for (const auto& record : objects.Roots())
    {
        Gather(objects, record, result);
    }

    return result;
}
} // namespace orbit::world_model
