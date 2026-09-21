#include <orbit/world_model/CelestialSmallBodyBinding.hpp>

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
    const auto value=objects.GetProperty(object,property);
    if(!value.has_value()) return fallback;
    const auto* typed=std::get_if<T>(&*value);
    if(typed==nullptr)
        throw std::runtime_error(
            "Small-body appearance property has unexpected type.");
    return *typed;
}
}

std::optional<ResolvedSmallBodyAppearance>
ResolveSmallBodyAppearance(
    const scene::ObjectStore& objects,
    const scene::ObjectId body)
{
    const auto bodyRecord=objects.Find(body);
    if(!bodyRecord.has_value() ||
       bodyRecord->type!=kCelestialBodyType)
        throw std::invalid_argument(
            "Small-body appearance binding requires a Celestial Body.");

    std::optional<scene::ObjectRecord> found;

    for(const auto& child:objects.Children(body))
    {
        if(child.type!=kSmallBodyAppearanceCapabilityType)
            continue;

        if(!PropertyOr<bool>(
                objects,child.id,kCapabilityEnabled,true))
            continue;

        if(found.has_value())
            throw std::runtime_error(
                "Body has multiple enabled Small Body Appearance capabilities.");

        found=child;
    }

    if(!found.has_value())
        return std::nullopt;

    const auto model=PropertyOr<std::string>(
        objects,found->id,kCapabilityModel,
        std::string{"Procedural Regolith"});

    if(model!="Procedural Regolith")
        throw std::runtime_error(
            "Unsupported Small Body Appearance model: "+model);

    const auto bodyClass=PropertyOr<std::string>(
        objects,found->id,kSmallBodyClass,
        std::string{"Asteroid"});

    celestial_small_bodies::SmallBodyClass resolvedClass{};
    if(bodyClass=="Asteroid")
        resolvedClass=celestial_small_bodies::SmallBodyClass::Asteroid;
    else if(bodyClass=="Comet Nucleus")
        resolvedClass=celestial_small_bodies::SmallBodyClass::CometNucleus;
    else if(bodyClass=="Moonlet")
        resolvedClass=celestial_small_bodies::SmallBodyClass::Moonlet;
    else
        throw std::runtime_error(
            "Small Body Class must be Asteroid, Comet Nucleus, or Moonlet.");

    celestial_small_bodies::SmallBodyParameters p{
        .bodyClass=resolvedClass,
        .seed=static_cast<u64>(std::max<i64>(
            PropertyOr<i64>(objects,found->id,kSmallBodySeed,i64{1}),0)),
        .axisScale=PropertyOr<math::Double3>(
            objects,found->id,kSmallBodyAxisScale,
            math::Double3{1.0,0.82,0.68}),
        .irregularity=PropertyOr<f64>(
            objects,found->id,kSmallBodyIrregularity,0.18),
        .largeLobeStrength=PropertyOr<f64>(
            objects,found->id,kSmallBodyLargeLobeStrength,0.12),
        .surfaceRoughness=PropertyOr<f64>(
            objects,found->id,kSmallBodySurfaceRoughness,0.92),
        .regolithColorLinear=PropertyOr<math::Double3>(
            objects,found->id,kSmallBodyRegolithColorLinear,
            math::Double3{0.16,0.145,0.13}),
        .freshMaterialColorLinear=PropertyOr<math::Double3>(
            objects,found->id,kSmallBodyFreshMaterialColorLinear,
            math::Double3{0.24,0.22,0.19}),
        .colorVariation=PropertyOr<f64>(
            objects,found->id,kSmallBodyColorVariation,0.18),
        .craterDensity=PropertyOr<f64>(
            objects,found->id,kSmallBodyCraterDensity,0.55),
        .craterDepth=PropertyOr<f64>(
            objects,found->id,kSmallBodyCraterDepth,0.12),
        .craterRimStrength=PropertyOr<f64>(
            objects,found->id,kSmallBodyCraterRimStrength,0.08),
        .oppositionStrength=PropertyOr<f64>(
            objects,found->id,kSmallBodyOppositionStrength,0.55),
        .oppositionWidthRadians=PropertyOr<f64>(
            objects,found->id,kSmallBodyOppositionWidthRadians,0.055),
        .singleScatteringAlbedo=PropertyOr<f64>(
            objects,found->id,kSmallBodySingleScatteringAlbedo,0.16),
        .macroscopicRoughnessRadians=PropertyOr<f64>(
            objects,found->id,kSmallBodyMacroscopicRoughnessRadians,0.42)
    };

    const u64 fingerprint=
        celestial_small_bodies::
            SmallBodyAppearanceFingerprint(p);

    return ResolvedSmallBodyAppearance{
        .body=body,
        .capability=found->id,
        .parameters=p,
        .fingerprint=fingerprint
    };
}
} // namespace orbit::world_model
