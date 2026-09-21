#include <orbit/world_model/CelestialOceanBinding.hpp>

#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <stdexcept>
#include <string>
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
    T fallback)
{
    const auto value =
        objects.GetProperty(object, property);

    if (!value.has_value())
        return fallback;

    const auto* typed =
        std::get_if<T>(&*value);

    if (typed == nullptr)
        throw std::runtime_error(
            "Ocean semantic property has unexpected type.");

    return *typed;
}
} // namespace

std::optional<ResolvedOceanBody>
ResolveOceanBody(
    const scene::ObjectStore& objects,
    const scene::ObjectId body)
{
    const auto bodyRecord =
        objects.Find(body);

    if (!bodyRecord.has_value() ||
        bodyRecord->type != kCelestialBodyType)
    {
        throw std::invalid_argument(
            "Ocean binding requires a Celestial Body.");
    }

    std::optional<scene::ObjectRecord> ocean;

    for (const auto& child :
         objects.Children(body))
    {
        if (child.type != kOceanCapabilityType)
            continue;

        if (!PropertyOr<bool>(
                objects,
                child.id,
                kCapabilityEnabled,
                true))
            continue;

        if (ocean.has_value())
        {
            throw std::runtime_error(
                "Celestial body has multiple enabled Ocean capabilities.");
        }

        ocean = child;
    }

    if (!ocean.has_value())
        return std::nullopt;

    const std::string model =
        PropertyOr<std::string>(
            objects,
            ocean->id,
            kCapabilityModel,
            std::string{"Surface Authority"});

    if (model != "Surface Authority")
    {
        throw std::runtime_error(
            "Unsupported Ocean model: " + model);
    }

    std::optional<scene::ObjectId> sourceObject;

    if (const auto source =
            objects.GetProperty(
                ocean->id,
                kCapabilitySourceObject);
        source.has_value())
    {
        if (const auto* ref =
                std::get_if<
                    schema::ObjectReferenceValue>(
                    &*source);
            ref != nullptr &&
            (ref->high != 0U ||
             ref->low != 0U))
        {
            sourceObject =
                scene::ObjectId{
                    .high = ref->high,
                    .low = ref->low
                };
        }
    }

    celestial_ocean::OceanOpticalParameters p{
        .refractiveIndex =
            PropertyOr<f64>(
                objects,
                ocean->id,
                kOceanRefractiveIndex,
                1.333),
        .orbitalRoughness =
            PropertyOr<f64>(
                objects,
                ocean->id,
                kOceanOrbitalRoughness,
                0.12),
        .absorptionPerMeter =
            PropertyOr<math::Double3>(
                objects,
                ocean->id,
                kOceanAbsorptionPerMeter,
                {0.18, 0.055, 0.025}),
        .deepWaterColor =
            PropertyOr<math::Double3>(
                objects,
                ocean->id,
                kOceanDeepWaterColor,
                {0.008, 0.035, 0.075}),
        .glintStrength =
            PropertyOr<f64>(
                objects,
                ocean->id,
                kOceanGlintStrength,
                1.0),
        .deepColorDepthMeters =
            PropertyOr<f64>(
                objects,
                ocean->id,
                kOceanMinimumDepthForDeepColorMeters,
                40.0)
    };

    static_cast<void>(
        celestial_ocean::
            OceanOpticalFingerprint(p));

    return ResolvedOceanBody{
        .body = body,
        .oceanCapability = ocean->id,
        .sourceObject = sourceObject,
        .optical = p
    };
}
} // namespace orbit::world_model
