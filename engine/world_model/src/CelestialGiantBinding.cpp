#include <orbit/world_model/CelestialGiantBinding.hpp>

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
        throw std::runtime_error("Giant appearance property has unexpected type.");
    return *typed;
}
}

std::optional<ResolvedGiantAppearance>
ResolveGiantAppearance(
    const scene::ObjectStore& objects,
    const scene::ObjectId body)
{
    const auto bodyRecord=objects.Find(body);
    if(!bodyRecord.has_value() ||
       bodyRecord->type!=kCelestialBodyType)
        throw std::invalid_argument("Giant appearance binding requires a Celestial Body.");

    std::optional<scene::ObjectRecord> found;

    for(const auto& child:objects.Children(body))
    {
        if(child.type!=kGiantAppearanceCapabilityType)
            continue;

        if(!PropertyOr<bool>(objects,child.id,kCapabilityEnabled,true))
            continue;

        if(found.has_value())
            throw std::runtime_error("Body has multiple enabled Giant Appearance capabilities.");

        found=child;
    }

    if(!found.has_value())
        return std::nullopt;

    const auto model=PropertyOr<std::string>(
        objects,found->id,kCapabilityModel,
        std::string{"Procedural Giant"});

    if(model!="Procedural Giant")
        throw std::runtime_error("Unsupported Giant Appearance model: "+model);

    const auto giantClass=PropertyOr<std::string>(
        objects,found->id,kGiantClass,
        std::string{"Gas Giant"});

    celestial_giants::GiantAppearanceParameters p{
        .giantClass=
            giantClass=="Ice Giant"
                ? celestial_giants::GiantClass::IceGiant
                : celestial_giants::GiantClass::GasGiant,
        .seed=static_cast<u64>(std::max<i64>(
            PropertyOr<i64>(objects,found->id,kGiantSeed,i64{1}),0)),
        .baseColorLinear=PropertyOr<math::Double3>(
            objects,found->id,kGiantBaseColorLinear,{0.62,0.48,0.31}),
        .bandColorLinear=PropertyOr<math::Double3>(
            objects,found->id,kGiantBandColorLinear,{0.90,0.78,0.58}),
        .polarColorLinear=PropertyOr<math::Double3>(
            objects,found->id,kGiantPolarColorLinear,{0.48,0.42,0.36}),
        .bandFrequency=PropertyOr<f64>(
            objects,found->id,kGiantBandFrequency,11.0),
        .bandStrength=PropertyOr<f64>(
            objects,found->id,kGiantBandStrength,0.72),
        .zonalShear=PropertyOr<f64>(
            objects,found->id,kGiantZonalShear,0.18),
        .stormStrength=PropertyOr<f64>(
            objects,found->id,kGiantStormStrength,0.35),
        .stormScale=PropertyOr<f64>(
            objects,found->id,kGiantStormScale,5.0),
        .polarStrength=PropertyOr<f64>(
            objects,found->id,kGiantPolarStrength,0.22),
        .depthContrast=PropertyOr<f64>(
            objects,found->id,kGiantDepthContrast,0.25),
        .turbulenceStrength=PropertyOr<f64>(
            objects,found->id,kGiantTurbulenceStrength,0.18),
        .turbulenceScale=PropertyOr<f64>(
            objects,found->id,kGiantTurbulenceScale,18.0)
    };

    const u64 fp=celestial_giants::GiantAppearanceFingerprint(p);

    return ResolvedGiantAppearance{
        .body=body,
        .capability=found->id,
        .parameters=p,
        .fingerprint=fp
    };
}
} // namespace orbit::world_model
