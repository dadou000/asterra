#include <orbit/world_model/CelestialMagnetosphereBinding.hpp>

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
    const auto value=
        objects.GetProperty(
            object,
            property);

    if(!value.has_value())
        return fallback;

    const auto* typed=
        std::get_if<T>(&*value);

    if(typed==nullptr)
        throw std::runtime_error(
            "Magnetosphere property has unexpected type.");

    return *typed;
}
}

std::optional<ResolvedMagnetosphere>
ResolveMagnetosphere(
    const scene::ObjectStore& objects,
    const scene::ObjectId body,
    const f64 referenceRadiusMeters)
{
    const auto bodyRecord=
        objects.Find(body);

    if(!bodyRecord.has_value() ||
       bodyRecord->type!=kCelestialBodyType)
        throw std::invalid_argument(
            "Magnetosphere binding requires a Celestial Body.");

    std::optional<scene::ObjectRecord> found;

    for(const auto& child:
        objects.Children(body))
    {
        if(child.type!=
           kMagnetosphereCapabilityType)
            continue;

        if(!PropertyOr<bool>(
                objects,
                child.id,
                kCapabilityEnabled,
                true))
            continue;

        if(found.has_value())
            throw std::runtime_error(
                "Body has multiple enabled Magnetosphere capabilities.");

        found=child;
    }

    if(!found.has_value())
        return std::nullopt;

    const auto model=
        PropertyOr<std::string>(
            objects,
            found->id,
            kCapabilityModel,
            std::string{"Parameterized Dipole"});

    if(model!="Parameterized Dipole")
        throw std::runtime_error(
            "Unsupported Magnetosphere model: "+
            model);

    celestial_magnetosphere::
        MagnetosphereParameters p{
            .dipoleAxis=
                PropertyOr<math::Double3>(
                    objects,
                    found->id,
                    kMagnetosphereDipoleAxis,
                    {0.0,0.0,1.0}),
            .equatorialFieldTesla=
                PropertyOr<f64>(
                    objects,
                    found->id,
                    kMagnetosphereEquatorialFieldTesla,
                    3.12e-5),
            .solarWindDirection=
                PropertyOr<math::Double3>(
                    objects,
                    found->id,
                    kMagnetosphereSolarWindDirection,
                    {-1.0,0.0,0.0}),
            .subsolarStandoffBodyRadii=
                PropertyOr<f64>(
                    objects,
                    found->id,
                    kMagnetosphereSubsolarStandoffBodyRadii,
                    10.2),
            .magnetopauseFlaringAlpha=
                PropertyOr<f64>(
                    objects,
                    found->id,
                    kMagnetosphereFlaringAlpha,
                    0.58),
            .maximumTailBodyRadii=
                PropertyOr<f64>(
                    objects,
                    found->id,
                    kMagnetosphereMaximumTailBodyRadii,
                    80.0),
            .solarWindDynamicPressurePascals=
                PropertyOr<f64>(
                    objects,
                    found->id,
                    kMagnetosphereSolarWindPressurePascals,
                    2.0e-9),
            .interplanetaryFieldBzTesla=
                PropertyOr<f64>(
                    objects,
                    found->id,
                    kMagnetosphereImfBzTesla,
                    0.0),
            .activity=
                PropertyOr<f64>(
                    objects,
                    found->id,
                    kMagnetosphereActivity,
                    0.35),
            .auroralOvalLatitudeDegrees=
                PropertyOr<f64>(
                    objects,
                    found->id,
                    kAuroraOvalLatitudeDegrees,
                    67.0),
            .auroralOvalWidthDegrees=
                PropertyOr<f64>(
                    objects,
                    found->id,
                    kAuroraOvalWidthDegrees,
                    7.0),
            .auroralMinimumAltitudeMeters=
                PropertyOr<f64>(
                    objects,
                    found->id,
                    kAuroraMinimumAltitudeMeters,
                    100000.0),
            .auroralMaximumAltitudeMeters=
                PropertyOr<f64>(
                    objects,
                    found->id,
                    kAuroraMaximumAltitudeMeters,
                    300000.0),
            .auroralIntensity=
                PropertyOr<f64>(
                    objects,
                    found->id,
                    kAuroraIntensity,
                    1.0),
            .auroralColorLinear=
                PropertyOr<math::Double3>(
                    objects,
                    found->id,
                    kAuroraColorLinear,
                    {0.12,1.8,0.42}),
            .auroralStructure=
                PropertyOr<f64>(
                    objects,
                    found->id,
                    kAuroraStructure,
                    0.65),
            .auroralSeed=
                static_cast<u64>(
                    std::max<i64>(
                        PropertyOr<i64>(
                            objects,
                            found->id,
                            kAuroraSeed,
                            i64{1}),
                        0))
        };

    const u64 fingerprint=
        celestial_magnetosphere::
            MagnetosphereFingerprint(
                p,
                referenceRadiusMeters);

    return ResolvedMagnetosphere{
        .body=body,
        .capability=found->id,
        .parameters=p,
        .fingerprint=fingerprint
    };
}
} // namespace orbit::world_model
