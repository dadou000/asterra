#pragma once

#include <orbit/celestial_clouds/SparseStormZones.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace orbit::celestial_clouds
{
// Sparse, body-fixed diagnostic perturbations shared between independent zones.
// This is NOT a conservative atmosphere field. The quantities below are
// dimensionless proxies and may only affect the experimental updraft diagnostic.
struct WeatherCrossZoneSource
{
    u64 zoneId{0};
    u64 childId{0};
    math::Double3 direction{1.0, 0.0, 0.0};
    f64 sourceSeconds{0.0};
    f64 radiusKm{0.0};
    f64 strength{0.0};
};

struct WeatherCrossZoneInfluence
{
    f64 lift{0.0};
    f64 undercut{0.0};
    u32 contributingSources{0};
};

struct WeatherCrossZoneConfig
{
    std::size_t maxSources{1024};
    f64 validitySeconds{900.0};
    f64 shellMinimumKm{2.0};
    f64 maximumStrength{1.0};
};

// Stateless spherical coordinate utility: +Y is north. Local east/north
// tangent coordinates are in kilometres. No longitude seam at +/-180 deg.
[[nodiscard]] inline math::Double3 WeatherLocalDirection(
    const math::Double3& centre, f64 eastKm, f64 northKm,
    f64 planetRadiusMeters) noexcept
{
    const auto c=StormZoneUnit(centre);
    const f64 r=std::max(1.0,planetRadiusMeters*0.001);
    const f64 horizontal=std::hypot(c.x,c.z);
    const math::Double3 east=horizontal>1e-9
        ? math::Double3{-c.z/horizontal,0.0,c.x/horizontal}
        : math::Double3{0.0,0.0,1.0};
    const math::Double3 north{
        c.y*east.z-c.z*east.y,
        c.z*east.x-c.x*east.z,
        c.x*east.y-c.y*east.x};
    // Exponential map approximation suitable for a bounded local domain.
    const f64 arc=std::hypot(eastKm,northKm)/r;
    if(arc<1e-12) return c;
    const f64 sn=std::sin(arc)/arc;
    return StormZoneUnit({
        c.x*std::cos(arc)+sn*(east.x*eastKm+north.x*northKm)/r,
        c.y*std::cos(arc)+sn*(east.y*eastKm+north.y*northKm)/r,
        c.z*std::cos(arc)+sn*(east.z*eastKm+north.z*northKm)/r
    });
}

// Immutable from consumer's perspective once published. Rebuild happens only
// after a simulation completion, never as a dense global grid every frame.
class WeatherCrossZoneField
{
public:
    explicit WeatherCrossZoneField(WeatherCrossZoneConfig config={})
        : config_(config) {}

    void Clear() { sources_.clear(); }

    // Existing source state is replaced atomically on the owner thread.
    // Order is canonical; duplicates from the same child keep the latest state.
    void Publish(std::vector<WeatherCrossZoneSource> proposed)
    {
        std::sort(proposed.begin(),proposed.end(),
            [](const auto& a,const auto& b)
            {
                if(a.zoneId!=b.zoneId) return a.zoneId<b.zoneId;
                if(a.childId!=b.childId) return a.childId<b.childId;
                return a.sourceSeconds>b.sourceSeconds;
            });
        std::vector<WeatherCrossZoneSource> accepted;
        accepted.reserve(std::min(proposed.size(),config_.maxSources));
        for(const auto& s:proposed)
        {
            if(accepted.size()>=config_.maxSources) break;
            if(s.zoneId==0 || s.childId==0 ||
                !std::isfinite(s.sourceSeconds) ||
                !std::isfinite(s.radiusKm) ||
                !std::isfinite(s.strength) ||
                s.radiusKm<=0 || s.strength<=0 ||
                !std::isfinite(s.direction.x) ||
                !std::isfinite(s.direction.y) ||
                !std::isfinite(s.direction.z)) continue;
            if(!accepted.empty() && accepted.back().zoneId==s.zoneId &&
                accepted.back().childId==s.childId) continue;
            auto candidate=s;
            candidate.direction=StormZoneUnit(candidate.direction);
            candidate.strength=std::clamp(candidate.strength,0.0,config_.maximumStrength);
            accepted.push_back(candidate);
        }
        sources_=std::move(accepted);
    }

    [[nodiscard]] WeatherCrossZoneInfluence Sample(
        const u64 destinationZone, const math::Double3& position,
        const f64 timeSeconds, const f64 planetRadiusMeters) const
    {
        WeatherCrossZoneInfluence outcome;
        if (!std::isfinite(timeSeconds) || !std::isfinite(planetRadiusMeters) ||
            planetRadiusMeters<=0) return outcome;
        const auto d=StormZoneUnit(position);
        const f64 radiusKm=planetRadiusMeters*0.001;
        for(const auto& s:sources_)
        {
            if(s.zoneId==destinationZone ||
                timeSeconds<s.sourceSeconds ||
                timeSeconds-s.sourceSeconds>config_.validitySeconds)
                continue;
            const f64 separation=std::acos(StormZoneDot(d,s.direction))*radiusKm;
            const f64 shell=std::max(config_.shellMinimumKm,0.15*s.radiusKm);
            if(separation>s.radiusKm+4.0*shell) continue;
            const f64 age=std::clamp(
                1.0-(timeSeconds-s.sourceSeconds)/std::max(1.0,config_.validitySeconds),
                0.0,1.0);
            const f64 edge=std::exp(-std::pow((separation-s.radiusKm)/shell,2));
            const f64 inside=separation<0.65*s.radiusKm ? 1.0 : 0.0;
            outcome.lift+=s.strength*age*edge;
            outcome.undercut+=s.strength*age*inside;
            ++outcome.contributingSources;
        }
        outcome.lift=std::clamp(outcome.lift,0.0,1.0);
        outcome.undercut=std::clamp(outcome.undercut,0.0,1.0);
        return outcome;
    }

    [[nodiscard]] std::size_t SourceCount() const noexcept { return sources_.size(); }

private:
    WeatherCrossZoneConfig config_;
    std::vector<WeatherCrossZoneSource> sources_;
};
} // namespace orbit::celestial_clouds
