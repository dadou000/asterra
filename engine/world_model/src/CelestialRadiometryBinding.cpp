#include <orbit/world_model/CelestialRadiometryBinding.hpp>

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
    const auto stored =
        objects.GetProperty(
            object,
            property);

    if (!stored.has_value())
    {
        return fallback;
    }

    const auto* value =
        std::get_if<T>(
            &*stored);

    if (value == nullptr)
    {
        throw std::runtime_error(
            "Radiative semantic property has unexpected type.");
    }

    return *value;
}

[[nodiscard]] bool Enabled(
    const scene::ObjectStore& objects,
    const scene::ObjectId object)
{
    return PropertyOr<bool>(
        objects,
        object,
        kCapabilityEnabled,
        true);
}
} // namespace

std::optional<ResolvedRadiativeBody>
ResolveRadiativeBody(
    const scene::ObjectStore& objects,
    const scene::ObjectId body)
{
    const auto bodyRecord =
        objects.Find(body);

    if (!bodyRecord.has_value() ||
        bodyRecord->type !=
            kCelestialBodyType)
    {
        throw std::invalid_argument(
            "Radiative binding requires a Celestial Body object.");
    }

    std::optional<scene::ObjectRecord> emitter;
    std::optional<scene::ObjectRecord> photosphere;

    for (const auto& child :
         objects.Children(body))
    {
        if (!Enabled(
                objects,
                child.id))
        {
            continue;
        }

        if (child.type ==
            kRadiativeEmitterCapabilityType)
        {
            if (emitter.has_value())
            {
                throw std::runtime_error(
                    "Celestial body has multiple enabled Radiative Emitter capabilities.");
            }

            emitter = child;
        }
        else if (
            child.type ==
            kPhotosphereCapabilityType)
        {
            if (photosphere.has_value())
            {
                throw std::runtime_error(
                    "Celestial body has multiple enabled Photosphere capabilities.");
            }

            photosphere = child;
        }
    }

    if (!emitter.has_value())
    {
        return std::nullopt;
    }

    const std::string model =
        PropertyOr<std::string>(
            objects,
            emitter->id,
            kCapabilityModel,
            std::string{"Blackbody"});

    if (model != "Blackbody")
    {
        throw std::runtime_error(
            "Unsupported Radiative Emitter model: " +
            model);
    }

    const bool deriveLuminosity =
        PropertyOr<bool>(
            objects,
            emitter->id,
            kEmitterDeriveLuminosity,
            true);

    f64 radiusMeters =
        PropertyOr<f64>(
            objects,
            body,
            kBodyRadius,
            1.0);

    f64 temperatureKelvin =
        PropertyOr<f64>(
            objects,
            emitter->id,
            kEmitterEffectiveTemperatureKelvin,
            5772.0);

    if (photosphere.has_value())
    {
        const std::string photosphereModel =
            PropertyOr<std::string>(
                objects,
                photosphere->id,
                kCapabilityModel,
                std::string{"Blackbody"});

        if (photosphereModel != "Blackbody")
        {
            throw std::runtime_error(
                "Unsupported Photosphere model: " +
                photosphereModel);
        }

        radiusMeters =
            PropertyOr<f64>(
                objects,
                photosphere->id,
                kPhotosphereRadiusMeters,
                radiusMeters);

        temperatureKelvin =
            PropertyOr<f64>(
                objects,
                photosphere->id,
                kPhotosphereTemperatureKelvin,
                temperatureKelvin);
    }

    const auto state =
        celestial_radiometry::Resolve({
            .radiusMeters =
                radiusMeters,
            .effectiveTemperatureKelvin =
                temperatureKelvin,
            .emissivity =
                PropertyOr<f64>(
                    objects,
                    emitter->id,
                    kEmitterEmissivity,
                    1.0),
            .explicitLuminosityWatts =
                PropertyOr<f64>(
                    objects,
                    emitter->id,
                    kEmitterLuminosityWatts,
                    0.0),
            .deriveLuminosity =
                deriveLuminosity
        });

    return ResolvedRadiativeBody{
        .body = body,
        .emitterCapability =
            emitter->id,
        .photosphereCapability =
            photosphere.has_value()
                ? std::optional(
                      photosphere->id)
                : std::nullopt,
        .radiative = state
    };
}
} // namespace orbit::world_model
