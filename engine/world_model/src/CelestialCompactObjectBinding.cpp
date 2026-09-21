#include <orbit/world_model/CelestialCompactObjectBinding.hpp>

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
    const auto value =
        objects.GetProperty(
            object,
            property);

    if (!value.has_value())
        return fallback;

    const auto* typed =
        std::get_if<T>(&*value);

    if (typed == nullptr)
        throw std::runtime_error(
            "Compact-object property has unexpected type.");

    return *typed;
}

[[nodiscard]] std::optional<scene::ObjectRecord>
FindEnabledCapability(
    const scene::ObjectStore& objects,
    const scene::ObjectId body,
    const schema::TypeId type)
{
    std::optional<scene::ObjectRecord>
        result;

    for (const auto& child :
         objects.Children(body))
    {
        if (child.type != type)
            continue;

        if (!PropertyOr<bool>(
                objects,
                child.id,
                kCapabilityEnabled,
                true))
            continue;

        if (result.has_value())
            throw std::runtime_error(
                "Body has multiple enabled singleton compact-object capabilities.");

        result = child;
    }

    return result;
}
} // namespace

std::optional<ResolvedCompactObject>
ResolveCompactObject(
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
            "Compact-object binding requires a Celestial Body.");
    }

    const auto found =
        FindEnabledCapability(
            objects,
            body,
            kCompactObjectCapabilityType);

    if (!found.has_value())
        return std::nullopt;

    const auto model =
        PropertyOr<std::string>(
            objects,
            found->id,
            kCapabilityModel,
            std::string{
                "Schwarzschild Baseline"});

    if (model !=
        "Schwarzschild Baseline")
    {
        throw std::runtime_error(
            "Unsupported Compact Object model: " +
            model);
    }

    celestial_compact_objects::
        CompactObjectParameters p{
            .model =
                celestial_compact_objects::
                    CompactObjectModel::
                        SchwarzschildBaseline,
            .gravitationalParameterM3PerS2 =
                PropertyOr<f64>(
                    objects,
                    found->id,
                    kCompactGravitationalParameter,
                    1.3271645321e20),
            .dimensionlessSpin =
                PropertyOr<f64>(
                    objects,
                    found->id,
                    kCompactDimensionlessSpin,
                    0.0),
            .spinAxis =
                PropertyOr<math::Double3>(
                    objects,
                    found->id,
                    kCompactSpinAxis,
                    {0.0, 0.0, 1.0}),
            .shadowScale =
                PropertyOr<f64>(
                    objects,
                    found->id,
                    kCompactShadowScale,
                    1.0),
            .lensingStrength =
                PropertyOr<f64>(
                    objects,
                    found->id,
                    kCompactLensingStrength,
                    1.0),
            .photonRingIntensity =
                PropertyOr<f64>(
                    objects,
                    found->id,
                    kCompactPhotonRingIntensity,
                    1.0)
        };

    return ResolvedCompactObject{
        .body = body,
        .capability = found->id,
        .parameters = p,
        .fingerprint =
            celestial_compact_objects::
                CompactObjectFingerprint(p)
    };
}

std::optional<ResolvedAccretionFlow>
ResolveAccretionFlow(
    const scene::ObjectStore& objects,
    const scene::ObjectId body,
    const celestial_compact_objects::
        CompactObjectParameters& compact)
{
    const auto bodyRecord =
        objects.Find(body);

    if (!bodyRecord.has_value() ||
        bodyRecord->type !=
            kCelestialBodyType)
    {
        throw std::invalid_argument(
            "Accretion-flow binding requires a Celestial Body.");
    }

    const auto found =
        FindEnabledCapability(
            objects,
            body,
            kAccretionFlowCapabilityType);

    if (!found.has_value())
        return std::nullopt;

    const auto model =
        PropertyOr<std::string>(
            objects,
            found->id,
            kCapabilityModel,
            std::string{
                "Thin Flow Baseline"});

    if (model !=
        "Thin Flow Baseline")
    {
        throw std::runtime_error(
            "Unsupported Accretion Flow model: " +
            model);
    }

    celestial_compact_objects::
        AccretionFlowParameters p{
            .innerRadiusRg =
                PropertyOr<f64>(
                    objects,
                    found->id,
                    kAccretionInnerRadiusRg,
                    6.0),
            .outerRadiusRg =
                PropertyOr<f64>(
                    objects,
                    found->id,
                    kAccretionOuterRadiusRg,
                    40.0),
            .axis =
                PropertyOr<math::Double3>(
                    objects,
                    found->id,
                    kAccretionAxis,
                    {0.0, 0.0, 1.0}),
            .colorLinear =
                PropertyOr<math::Double3>(
                    objects,
                    found->id,
                    kAccretionColorLinear,
                    {1.4, 0.42, 0.08}),
            .intensity =
                PropertyOr<f64>(
                    objects,
                    found->id,
                    kAccretionIntensity,
                    1.0),
            .temperatureKelvin =
                PropertyOr<f64>(
                    objects,
                    found->id,
                    kAccretionTemperatureKelvin,
                    8.0e6),
            .radialFalloffExponent =
                PropertyOr<f64>(
                    objects,
                    found->id,
                    kAccretionRadialFalloffExponent,
                    2.0),
            .thicknessRatio =
                PropertyOr<f64>(
                    objects,
                    found->id,
                    kAccretionThicknessRatio,
                    0.08),
            .dopplerStrength =
                PropertyOr<f64>(
                    objects,
                    found->id,
                    kAccretionDopplerStrength,
                    0.45),
            .seed =
                static_cast<u64>(
                    std::max<i64>(
                        PropertyOr<i64>(
                            objects,
                            found->id,
                            kAccretionSeed,
                            i64{1}),
                        0))
        };

    return ResolvedAccretionFlow{
        .body = body,
        .capability = found->id,
        .parameters = p,
        .fingerprint =
            celestial_compact_objects::
                AccretionFlowFingerprint(
                    p,
                    compact)
    };
}
} // namespace orbit::world_model
