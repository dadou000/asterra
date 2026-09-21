#include <orbit/world_model/LocalLightBinding.hpp>

#include <orbit/world_model/WorldSchemas.hpp>

#include <cmath>
#include <optional>
#include <vector>

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

void Gather(
    const scene::ObjectStore& objects,
    const scene::ObjectRecord& record,
    std::vector<AuthoredLocalLight>& result)
{
    const bool point =
        record.type == kPointLightType;
    const bool spot =
        record.type == kSpotLightType;

    if (point || spot)
    {
        const bool enabled =
            PropertyOr<bool>(
                objects,
                record.id,
                kLightEnabled,
                true);

        if (enabled)
        {
            const auto position =
                PropertyOr<math::Double3>(
                    objects,
                    record.id,
                    kLightPositionMeters,
                    {});

            const auto color =
                PropertyOr<math::Double3>(
                    objects,
                    record.id,
                    kLightColorLinear,
                    {1.0, 1.0, 1.0});

            const f64 lumens =
                PropertyOr<f64>(
                    objects,
                    record.id,
                    kLightIntensityLumens,
                    point ? 800.0 : 1200.0);

            const f64 range =
                PropertyOr<f64>(
                    objects,
                    record.id,
                    kLightRangeMeters,
                    point ? 12.0 : 20.0);

            if (std::isfinite(lumens) &&
                std::isfinite(range) &&
                lumens > 0.0 &&
                range > 0.0)
            {
                AuthoredLocalLight light{
                    .object = record.id,
                    .kind =
                        point
                            ? AuthoredLightKind::Point
                            : AuthoredLightKind::Spot,
                    .positionMeters = position,
                    .direction =
                        PropertyOr<math::Double3>(
                            objects,
                            record.id,
                            kLightDirection,
                            {0.0, -1.0, 0.0}),
                    .colorLinear = color,
                    .luminousFluxLumens = lumens,
                    .rangeMeters = range
                };

                if (spot)
                {
                    light.innerConeDegrees =
                        PropertyOr<f64>(
                            objects,
                            record.id,
                            kLightInnerConeDegrees,
                            22.5);
                    light.outerConeDegrees =
                        PropertyOr<f64>(
                            objects,
                            record.id,
                            kLightOuterConeDegrees,
                            35.0);
                }

                result.push_back(light);
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

std::vector<AuthoredLocalLight>
ResolveAuthoredLocalLights(
    const scene::ObjectStore& objects,
    const std::optional<scene::ObjectId> root)
{
    std::vector<AuthoredLocalLight> result;

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

    for (const auto& rootRecord :
         objects.Roots())
    {
        Gather(
            objects,
            rootRecord,
            result);
    }

    return result;
}
} // namespace orbit::world_model
