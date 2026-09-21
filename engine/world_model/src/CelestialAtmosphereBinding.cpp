#include <orbit/world_model/CelestialAtmosphereBinding.hpp>

#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
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
            "Atmosphere semantic property has unexpected type.");
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

std::optional<ResolvedAtmosphereBody>
ResolveAtmosphereBody(
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
            "Atmosphere binding requires a Celestial Body object.");
    }

    std::optional<scene::ObjectRecord>
        atmosphere;

    for (const auto& child :
         objects.Children(body))
    {
        if (child.type !=
                kAtmosphereCapabilityType ||
            !Enabled(
                objects,
                child.id))
        {
            continue;
        }

        if (atmosphere.has_value())
        {
            throw std::runtime_error(
                "Celestial body has multiple enabled Atmosphere capabilities.");
        }

        atmosphere = child;
    }

    if (!atmosphere.has_value())
    {
        return std::nullopt;
    }

    const std::string model =
        PropertyOr<std::string>(
            objects,
            atmosphere->id,
            kCapabilityModel,
            std::string{
                "Physical Scattering"});

    if (model !=
        "Physical Scattering")
    {
        throw std::runtime_error(
            "Unsupported Atmosphere model: " +
            model);
    }

    celestial_atmosphere::
        AtmosphereParameters parameters;

    parameters.bottomRadiusMeters =
        PropertyOr<f64>(
            objects,
            body,
            kBodyRadius,
            parameters.
                bottomRadiusMeters);

    parameters.topRadiusMeters =
        PropertyOr<f64>(
            objects,
            atmosphere->id,
            kAtmosphereTopRadiusMeters,
            parameters.bottomRadiusMeters +
                std::max(
                    80'000.0,
                    parameters.bottomRadiusMeters *
                        0.012));

    parameters.rayleighScatteringPerMeter =
        PropertyOr<math::Double3>(
            objects,
            atmosphere->id,
            kAtmosphereRayleighScatteringPerMeter,
            parameters.
                rayleighScatteringPerMeter);

    parameters.rayleighScaleHeightMeters =
        PropertyOr<f64>(
            objects,
            atmosphere->id,
            kAtmosphereRayleighScaleHeightMeters,
            parameters.
                rayleighScaleHeightMeters);

    parameters.mieScatteringPerMeter =
        PropertyOr<math::Double3>(
            objects,
            atmosphere->id,
            kAtmosphereMieScatteringPerMeter,
            parameters.
                mieScatteringPerMeter);

    parameters.mieExtinctionPerMeter =
        PropertyOr<math::Double3>(
            objects,
            atmosphere->id,
            kAtmosphereMieExtinctionPerMeter,
            parameters.
                mieExtinctionPerMeter);

    parameters.mieScaleHeightMeters =
        PropertyOr<f64>(
            objects,
            atmosphere->id,
            kAtmosphereMieScaleHeightMeters,
            parameters.
                mieScaleHeightMeters);

    parameters.mieAnisotropy =
        PropertyOr<f64>(
            objects,
            atmosphere->id,
            kAtmosphereMieAnisotropy,
            parameters.
                mieAnisotropy);

    parameters.absorptionExtinctionPerMeter =
        PropertyOr<math::Double3>(
            objects,
            atmosphere->id,
            kAtmosphereAbsorptionExtinctionPerMeter,
            parameters.
                absorptionExtinctionPerMeter);

    parameters.absorptionCenterHeightMeters =
        PropertyOr<f64>(
            objects,
            atmosphere->id,
            kAtmosphereAbsorptionCenterHeightMeters,
            parameters.
                absorptionCenterHeightMeters);

    parameters.absorptionHalfWidthMeters =
        PropertyOr<f64>(
            objects,
            atmosphere->id,
            kAtmosphereAbsorptionHalfWidthMeters,
            parameters.
                absorptionHalfWidthMeters);

    parameters.groundAlbedo =
        PropertyOr<math::Double3>(
            objects,
            atmosphere->id,
            kAtmosphereGroundAlbedo,
            parameters.
                groundAlbedo);

    // Force validation through the canonical fingerprint function.
    static_cast<void>(
        celestial_atmosphere::
            AtmosphereFingerprint(
                parameters));

    return ResolvedAtmosphereBody{
        .body = body,
        .atmosphereCapability =
            atmosphere->id,
        .parameters =
            parameters
    };
}
} // namespace orbit::world_model
