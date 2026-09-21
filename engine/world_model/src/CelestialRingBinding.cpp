#include <orbit/world_model/CelestialRingBinding.hpp>

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
        throw std::runtime_error("Ring semantic property has unexpected type.");
    return *typed;
}
} // namespace

std::optional<ResolvedRingSystem>
ResolveRingSystem(
    const scene::ObjectStore& objects,
    const scene::ObjectId body)
{
    const auto bodyRecord=objects.Find(body);
    if(!bodyRecord.has_value() ||
       bodyRecord->type!=kCelestialBodyType)
        throw std::invalid_argument("Ring binding requires a Celestial Body.");

    std::optional<scene::ObjectRecord> system;
    for(const auto& child:objects.Children(body))
    {
        if(child.type!=kRingSystemCapabilityType)
            continue;
        if(!PropertyOr<bool>(objects,child.id,kCapabilityEnabled,true))
            continue;
        if(system.has_value())
            throw std::runtime_error("Body has multiple enabled Ring Systems.");
        system=child;
    }
    if(!system.has_value())
        return std::nullopt;

    const auto model=PropertyOr<std::string>(
        objects,system->id,kCapabilityModel,
        std::string{"Particle Distribution"});
    if(model!="Particle Distribution")
        throw std::runtime_error("Unsupported Ring System model: "+model);

    celestial_rings::RingSystem p{
        .planeNormalBody=PropertyOr<math::Double3>(
            objects,system->id,kRingPlaneNormalBody,{0.0,1.0,0.0}),
        .castRingShadows=PropertyOr<bool>(
            objects,system->id,kRingShadowParticipation,true),
        .receiveBodyShadow=PropertyOr<bool>(
            objects,system->id,kRingBodyShadowEnabled,true)
    };

    for(const auto& band:objects.Children(system->id))
    {
        if(band.type!=kRingBandType)
            continue;
        if(!PropertyOr<bool>(objects,band.id,kCapabilityEnabled,true))
            continue;

        const auto bandModel=PropertyOr<std::string>(
            objects,band.id,kCapabilityModel,std::string{"Physical Band"});
        if(bandModel!="Physical Band")
            throw std::runtime_error("Unsupported Ring Band model: "+bandModel);

        p.bands.push_back({
            .semanticIdHigh=band.id.high,
            .semanticIdLow=band.id.low,
            .innerRadiusMeters=PropertyOr<f64>(
                objects,band.id,kRingBandInnerRadiusMeters,8.0e7),
            .outerRadiusMeters=PropertyOr<f64>(
                objects,band.id,kRingBandOuterRadiusMeters,1.4e8),
            .normalOpticalDepth=PropertyOr<f64>(
                objects,band.id,kRingBandOpticalDepth,0.7),
            .singleScatteringAlbedo=PropertyOr<f64>(
                objects,band.id,kRingBandSingleScatteringAlbedo,0.65),
            .anisotropy=PropertyOr<f64>(
                objects,band.id,kRingBandAnisotropy,0.35),
            .colorLinear=PropertyOr<math::Double3>(
                objects,band.id,kRingBandColorLinear,{0.72,0.66,0.56}),
            .thicknessMeters=PropertyOr<f64>(
                objects,band.id,kRingBandThicknessMeters,100.0)
        });
    }

    std::sort(
        p.bands.begin(),p.bands.end(),
        [](const auto& a,const auto& b)
        {
            if(a.innerRadiusMeters!=b.innerRadiusMeters)
                return a.innerRadiusMeters<b.innerRadiusMeters;
            if(a.semanticIdHigh!=b.semanticIdHigh)
                return a.semanticIdHigh<b.semanticIdHigh;
            return a.semanticIdLow<b.semanticIdLow;
        });

    p.fingerprint=celestial_rings::RingSystemFingerprint(p);

    return ResolvedRingSystem{
        .body=body,
        .ringSystem=system->id,
        .parameters=std::move(p)
    };
}
} // namespace orbit::world_model
