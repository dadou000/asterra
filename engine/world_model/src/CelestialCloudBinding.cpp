#include <orbit/world_model/CelestialCloudBinding.hpp>

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
        objects.GetProperty(object, property);

    if (!value.has_value())
        return fallback;

    const auto* typed =
        std::get_if<T>(&*value);

    if (typed == nullptr)
        throw std::runtime_error(
            "Cloud semantic property has unexpected type.");

    return *typed;
}

[[nodiscard]] celestial_clouds::CloudSourceModel
ParseSourceModel(const std::string& value)
{
    if (value == "Climate Procedural")
        return celestial_clouds::CloudSourceModel::ClimateProcedural;
    if (value == "Procedural")
        return celestial_clouds::CloudSourceModel::Procedural;
    if (value == "Authored")
        return celestial_clouds::CloudSourceModel::Authored;
    if (value == "Imported")
        return celestial_clouds::CloudSourceModel::Imported;

    throw std::runtime_error(
        "Unsupported Cloud Source Model: " + value);
}
} // namespace

std::vector<ResolvedCloudLayer>
ResolveCloudLayers(
    const scene::ObjectStore& objects,
    const scene::ObjectId body)
{
    const auto bodyRecord =
        objects.Find(body);

    if (!bodyRecord.has_value() ||
        bodyRecord->type != kCelestialBodyType)
    {
        throw std::invalid_argument(
            "Cloud binding requires a Celestial Body.");
    }

    std::vector<ResolvedCloudLayer> result;

    for (const auto& child :
         objects.Children(body))
    {
        if (child.type !=
            kCloudLayerCapabilityType)
        {
            continue;
        }

        const bool enabled =
            PropertyOr<bool>(
                objects,
                child.id,
                kCapabilityEnabled,
                true);

        if (!enabled)
            continue;

        const std::string model =
            PropertyOr<std::string>(
                objects,
                child.id,
                kCapabilityModel,
                std::string{"Volumetric Layer"});

        if (model != "Volumetric Layer")
        {
            throw std::runtime_error(
                "Unsupported Cloud Layer model: " + model);
        }

        std::optional<scene::ObjectId>
            sourceObject;

        if (const auto ref =
                objects.GetProperty(
                    child.id,
                    kCapabilitySourceObject);
            ref.has_value())
        {
            if (const auto* value =
                    std::get_if<
                        schema::ObjectReferenceValue>(
                        &*ref);
                value != nullptr &&
                (value->high != 0U ||
                 value->low != 0U))
            {
                sourceObject =
                    scene::ObjectId{
                        .high = value->high,
                        .low = value->low
                    };
            }
        }

        celestial_clouds::CloudLayerParameters p{
            .semanticIdHigh = child.id.high,
            .semanticIdLow = child.id.low,
            .sourceModel =
                ParseSourceModel(
                    PropertyOr<std::string>(
                        objects,
                        child.id,
                        kCloudSourceModel,
                        std::string{
                            "Climate Procedural"})),
            .baseAltitudeMeters =
                PropertyOr<f64>(
                    objects,
                    child.id,
                    kCloudBaseAltitudeMeters,
                    1500.0),
            .topAltitudeMeters =
                PropertyOr<f64>(
                    objects,
                    child.id,
                    kCloudTopAltitudeMeters,
                    6500.0),
            .coverageBias =
                PropertyOr<f64>(
                    objects,
                    child.id,
                    kCloudCoverageBias,
                    0.0),
            .peakOpticalDepth =
                PropertyOr<f64>(
                    objects,
                    child.id,
                    kCloudOpticalDepth,
                    8.0),
            .singleScatteringAlbedo =
                PropertyOr<f64>(
                    objects,
                    child.id,
                    kCloudSingleScatteringAlbedo,
                    0.999),
            .anisotropy =
                PropertyOr<f64>(
                    objects,
                    child.id,
                    kCloudAnisotropy,
                    0.72),
            .densityExponent =
                PropertyOr<f64>(
                    objects,
                    child.id,
                    kCloudDensityExponent,
                    1.35),
            .weatherScale =
                PropertyOr<f64>(
                    objects,
                    child.id,
                    kCloudWeatherScale,
                    3.5),
            .detailScale =
                PropertyOr<f64>(
                    objects,
                    child.id,
                    kCloudDetailScale,
                    14.0),
            .seed =
                static_cast<u64>(
                    std::max<i64>(
                        PropertyOr<i64>(
                            objects,
                            child.id,
                            kCloudSeed,
                            i64{1}),
                        0)),
            .windAngularRadiansPerSecond =
                PropertyOr<math::Double3>(
                    objects,
                    child.id,
                    kCloudWindAngularRadiansPerSecond,
                    math::Double3{
                        0.0, 0.0, 7.272205e-6}),
            .shadowParticipation =
                PropertyOr<bool>(
                    objects,
                    child.id,
                    kCloudShadowParticipation,
                    true),
            .orbitalRepresentation =
                PropertyOr<bool>(
                    objects,
                    child.id,
                    kCloudOrbitalRepresentation,
                    true)
        };

        result.push_back({
            .body = body,
            .capability = child.id,
            .sourceObject = sourceObject,
            .parameters = p
        });
    }

    std::sort(
        result.begin(),
        result.end(),
        [](const ResolvedCloudLayer& a,
           const ResolvedCloudLayer& b)
        {
            return a.capability <
                   b.capability;
        });

    return result;
}
} // namespace orbit::world_model
