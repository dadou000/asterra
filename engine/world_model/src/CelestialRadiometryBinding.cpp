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

    celestial_stellar::StellarAppearanceParameters
        stellarAppearance{
            .effectiveTemperatureKelvin =
                temperatureKelvin,
            .limbDarkening =
                photosphere.has_value()
                    ? PropertyOr<f64>(
                          objects,
                          photosphere->id,
                          kPhotosphereLimbDarkening,
                          0.58)
                    : 0.58,
            .granulationStrength =
                photosphere.has_value()
                    ? PropertyOr<f64>(
                          objects,
                          photosphere->id,
                          kPhotosphereGranulationStrength,
                          0.10)
                    : 0.10,
            .granulationScale =
                photosphere.has_value()
                    ? PropertyOr<f64>(
                          objects,
                          photosphere->id,
                          kPhotosphereGranulationScale,
                          42.0)
                    : 42.0,
            .activityLevel =
                photosphere.has_value()
                    ? PropertyOr<f64>(
                          objects,
                          photosphere->id,
                          kPhotosphereActivityLevel,
                          0.12)
                    : 0.12,
            .activitySeed =
                static_cast<u64>(
                    std::max<i64>(
                        photosphere.has_value()
                            ? PropertyOr<i64>(
                                  objects,
                                  photosphere->id,
                                  kPhotosphereActivitySeed,
                                  i64{1})
                            : i64{1},
                        0)),
            .chromosphereStrength =
                photosphere.has_value()
                    ? PropertyOr<f64>(
                          objects,
                          photosphere->id,
                          kPhotosphereChromosphereStrength,
                          0.08)
                    : 0.08,
            .chromosphereExtent =
                photosphere.has_value()
                    ? PropertyOr<f64>(
                          objects,
                          photosphere->id,
                          kPhotosphereChromosphereExtent,
                          0.035)
                    : 0.035,
            .coronaStrength =
                photosphere.has_value()
                    ? PropertyOr<f64>(
                          objects,
                          photosphere->id,
                          kPhotosphereCoronaStrength,
                          0.025)
                    : 0.025,
            .coronaExtent =
                photosphere.has_value()
                    ? PropertyOr<f64>(
                          objects,
                          photosphere->id,
                          kPhotosphereCoronaExtent,
                          1.75)
                    : 1.75,
            .glareStrength =
                photosphere.has_value()
                    ? PropertyOr<f64>(
                          objects,
                          photosphere->id,
                          kPhotosphereGlareStrength,
                          0.35)
                    : 0.35,
            .glareRadiusPixels =
                photosphere.has_value()
                    ? PropertyOr<f64>(
                          objects,
                          photosphere->id,
                          kPhotosphereGlareRadiusPixels,
                          5.0)
                    : 5.0
        };

    const auto stellarColor =
        celestial_stellar::
            BlackbodyColorLinear(
                temperatureKelvin);

    const u64 stellarFingerprint =
        celestial_stellar::
            StellarAppearanceFingerprint(
                stellarAppearance);

    return ResolvedRadiativeBody{
        .body = body,
        .emitterCapability =
            emitter->id,
        .photosphereCapability =
            photosphere.has_value()
                ? std::optional(
                      photosphere->id)
                : std::nullopt,
        .photosphereRadiusMeters =
            radiusMeters,
        .radiative = state,
        .stellarAppearance =
            stellarAppearance,
        .stellarColorLinear =
            stellarColor,
        .stellarAppearanceFingerprint =
            stellarFingerprint
    };
}
} // namespace orbit::world_model
