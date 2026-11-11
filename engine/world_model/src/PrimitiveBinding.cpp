#include <orbit/world_model/PrimitiveBinding.hpp>

#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
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
            const i64 surface =
                PropertyOr<i64>(
                    objects, record.id, kPrimitiveSurface, i64{0});
            const auto color =
                PropertyOr<math::Double3>(
                    objects, record.id, kPrimitiveColor,
                    {0.8, 0.8, 0.8});
            const auto unit = [](const f64 value, const f64 fallback)
            {
                return std::isfinite(value)
                           ? std::clamp(value, 0.0, 1.0)
                           : fallback;
            };
            const auto clampedColor = Finite(color)
                ? math::Double3{
                      std::clamp(color.x, 0.0, 16.0),
                      std::clamp(color.y, 0.0, 16.0),
                      std::clamp(color.z, 0.0, 16.0)}
                : math::Double3{0.8, 0.8, 0.8};
            const f64 ior =
                PropertyOr<f64>(
                    objects, record.id, kPrimitiveIor, 1.5);
            const f64 emissionRaw =
                PropertyOr<f64>(
                    objects, record.id, kPrimitiveEmissionNits, 100000.0);
            const f64 emission = std::isfinite(emissionRaw)
                ? std::max(emissionRaw, 0.0)
                : 0.0;

            result.push_back({
                .object = record.id,
                .shape = static_cast<PrimitiveShape>(shape),
                .positionMeters = position,
                .eulerDegrees = euler,
                .sizeMeters = size,
                .surface = surface >= 0 && surface <= 3
                    ? static_cast<PrimitiveSurface>(surface)
                    : PrimitiveSurface::Standard,
                .color = clampedColor,
                .roughness = unit(
                    PropertyOr<f64>(
                        objects, record.id, kPrimitiveRoughness, 0.5),
                    0.5),
                .metallic = unit(
                    PropertyOr<f64>(
                        objects, record.id, kPrimitiveMetallic, 0.0),
                    0.0),
                .indexOfRefraction = std::isfinite(ior)
                    ? std::clamp(ior, 1.0, 3.0)
                    : 1.5,
                .caustics = PropertyOr<bool>(
                    objects, record.id, kPrimitiveCaustics, true),
                .emissionNits = emission,
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

namespace orbit::world_model
{
PrimitiveCreateRequest MakePrimitivePreset(
    const PrimitiveShape shape,
    const PrimitiveSurface surface)
{
    PrimitiveCreateRequest request;
    request.shape = shape;
    request.surface = surface;
    switch (surface)
    {
    case PrimitiveSurface::Mirror:
        request.color = {1.0, 1.0, 1.0};
        request.metallic = 1.0;
        request.roughness = 0.0;
        break;
    case PrimitiveSurface::Glass:
        request.color = {1.0, 1.0, 1.0};
        request.roughness = 0.0;
        request.indexOfRefraction = 1.5;
        break;
    case PrimitiveSurface::Emissive:
        request.color = {1.0, 0.92, 0.78};
        request.emissionNits = 100000.0;
        break;
    case PrimitiveSurface::Standard:
        break;
    }

    const char* surfaceName = "";
    switch (surface)
    {
    case PrimitiveSurface::Mirror: surfaceName = "Mirror "; break;
    case PrimitiveSurface::Glass: surfaceName = "Glass "; break;
    case PrimitiveSurface::Emissive: surfaceName = "Emissive "; break;
    case PrimitiveSurface::Standard: break;
    }
    const char* shapeName = "Box";
    switch (shape)
    {
    case PrimitiveShape::Box: shapeName = "Box"; break;
    case PrimitiveShape::Sphere: shapeName = "Sphere"; break;
    case PrimitiveShape::Cylinder: shapeName = "Cylinder"; break;
    case PrimitiveShape::Capsule: shapeName = "Capsule"; break;
    case PrimitiveShape::Plane: shapeName = "Plane"; break;
    }
    request.name = std::string(surfaceName) + shapeName;
    if (shape == PrimitiveShape::Plane)
    {
        request.sizeMeters = {2.0, 1.0, 2.0};
    }
    return request;
}

scene::ObjectId CreatePrimitive(
    commands::CommandService& commands,
    const scene::ObjectId parent,
    const PrimitiveCreateRequest& request)
{
    const std::string transactionName = "Add " + request.name;
    commands.BeginTransaction(transactionName);
    try
    {
        const auto created = commands.CreateObject(
            kPrimitiveType,
            request.name.empty() ? std::string("Primitive") : request.name,
            parent);

        commands.SetProperty(
            created, kPrimitiveShape, i64{static_cast<i64>(request.shape)});
        commands.SetProperty(
            created, kPrimitiveSurface, i64{static_cast<i64>(request.surface)});
        commands.SetProperty(
            created, kPrimitivePositionMeters, request.positionMeters);
        commands.SetProperty(
            created, kPrimitiveEulerDegrees, request.eulerDegrees);
        commands.SetProperty(
            created, kPrimitiveSizeMeters, request.sizeMeters);
        commands.SetProperty(created, kPrimitiveColor, request.color);
        commands.SetProperty(created, kPrimitiveRoughness, request.roughness);
        commands.SetProperty(created, kPrimitiveMetallic, request.metallic);
        commands.SetProperty(
            created, kPrimitiveIor, request.indexOfRefraction);
        commands.SetProperty(created, kPrimitiveCaustics, request.caustics);
        commands.SetProperty(
            created, kPrimitiveEmissionNits, request.emissionNits);

        commands.CommitTransaction();
        return created;
    }
    catch (...)
    {
        if (commands.HasActiveTransaction())
        {
            commands.RollbackTransaction();
        }
        throw;
    }
}
} // namespace orbit::world_model
