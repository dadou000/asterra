#pragma once

#include <orbit/celestial_clouds/WeatherModel.hpp>
#include <orbit/core/Types.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>
#include <utility>

namespace orbit::celestial_clouds
{
// Experimental sparse discovery layer. A probe is a cheap opportunity observation supplied
// by ANY planetary provider. No global grid of derived storm-type weights is allocated.
// Coordinates are body-fixed unit directions, consistently with WeatherModel.
struct StormZoneProbe
{
    math::Double3 direction{1.0, 0.0, 0.0};
    f32 opportunity{0.0F};  // cheap screening value; NOT a storm probability
    f32 moisture{0.0F};
    f32 convection{0.0F};
    f32 precipitation{0.0F};
};

// A tiny, shared diagnostic profile. These are independent suitability indicators,
// not exclusive probabilities and NEVER cause storms to be spawned.
struct StormZoneWeights
{
    f32 initiation{0.0F};
    f32 convection{0.0F};
    f32 multicell{0.0F};
    f32 organized{0.0F};
    f32 moisture{0.0F};
    f32 precipitation{0.0F};
};

struct StormZone
{
    u64 id{0};
    math::Double3 centre{1.0, 0.0, 0.0};
    f64 radiusRadians{0.0}; // discovery extent, not a physical storm boundary
    f64 lastSeenSeconds{0.0};
    StormZoneWeights weights{};
    u32 contributingProbes{0};
};

// Discovery runs at the global solver's own cadence, not at render-frame rate.
// Retire hysteresis keeps existing convective entities from losing their zone on a dip.
struct StormZoneConfig
{
    f32 enterThreshold{0.62F};
    f32 retainThreshold{0.42F};
    f64 neighbourRadians{0.035};  // approximately 220 km on Earth
    f64 matchRadians{0.08};
    f64 retireAfterSeconds{3600.0};
    std::size_t maxProbes{8192};
    std::size_t maxZones{256};
};

[[nodiscard]] inline f64 StormZoneDot(const math::Double3& a, const math::Double3& b) noexcept
{
    return std::clamp(a.x*b.x + a.y*b.y + a.z*b.z, -1.0, 1.0);
}

[[nodiscard]] inline math::Double3 StormZoneUnit(const math::Double3& a) noexcept
{
    const f64 length = std::sqrt(a.x*a.x + a.y*a.y + a.z*a.z);
    return length > 1.0e-10 && std::isfinite(length)
        ? math::Double3{a.x/length, a.y/length, a.z/length}
        : math::Double3{1.0, 0.0, 0.0};
}

[[nodiscard]] inline f32 StormZoneClamp(const f32 x) noexcept
{
    return std::isfinite(x) ? std::clamp(x, 0.0F, 1.0F) : 0.0F;
}

// Compatibility bridge for today's analytic map. These proxies do NOT provide
// wind shear, CAPE, CIN or actual meteorological supercell suitability.
// A physical global solver should supply its own low-cost probes and diagnostic provider.
[[nodiscard]] inline StormZoneProbe AnalyticStormProbe(
    const math::Double3& direction,
    const f64 seconds,
    const WeatherParameters& weather,
    const WeatherClimate& climate)
{
    const math::Double3 d = StormZoneUnit(direction);
    const WeatherState state = EvaluateWeather(d, seconds, weather, climate);
    const f32 deep = StormZoneClamp(static_cast<f32>(
        state.coverage * std::clamp((state.cloudType - 0.45) / 0.40, 0.0, 1.0)));
    return {d, StormZoneClamp(0.8F*deep + 0.2F*static_cast<f32>(state.precipitation)),
        StormZoneClamp(static_cast<f32>(climate.valid ? climate.humidity : 0.5)),
        deep, StormZoneClamp(static_cast<f32>(state.precipitation))};
}

class SparseStormZones
{
public:
    explicit SparseStormZones(StormZoneConfig config = {}) : config_(config) {}

    // Caller provides only coarse candidate probes, not all atmospheric cells.
    // Probe order is irrelevant when the input is within maxProbes; excess probes
    // are discarded in caller order to enforce a hard work budget.
    // Non-monotonic simulated time resets identity tracking; historical reconstruction
    // belongs to the future event/checkpoint scheduler.
    void Discover(const std::vector<StormZoneProbe>& probes, f64 seconds)
    {
        if (!std::isfinite(seconds)) { return; }
        if (seconds < lastTime_) { zones_.clear(); }
        lastTime_ = seconds;

        struct Candidate { StormZoneProbe p; bool active; };
        std::vector<Candidate> candidates;
        candidates.reserve(std::min(probes.size(), config_.maxProbes));
        for (const auto& raw : probes)
        {
            if (candidates.size() >= config_.maxProbes) { break; }
            const f32 opportunity = StormZoneClamp(raw.opportunity);
            // Retain lower-scoring probes ONLY in the neighborhood of an existing zone.
            bool retain = false;
            if (opportunity >= config_.retainThreshold)
            {
                for (const auto& zone : zones_)
                {
                    if (StormZoneDot(StormZoneUnit(raw.direction), zone.centre) >=
                        std::cos(config_.matchRadians)) { retain = true; break; }
                }
            }
            if (opportunity < config_.enterThreshold && !retain) { continue; }
            Candidate c{raw, opportunity >= config_.enterThreshold};
            c.p.direction = StormZoneUnit(raw.direction);
            c.p.opportunity = opportunity;
            candidates.push_back(c);
        }
        // Deterministic order independent of caller sampling order.
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b)
        {
            if (a.p.direction.x != b.p.direction.x) return a.p.direction.x < b.p.direction.x;
            if (a.p.direction.y != b.p.direction.y) return a.p.direction.y < b.p.direction.y;
            return a.p.direction.z < b.p.direction.z;
        });
        const std::size_t n = candidates.size();
        std::vector<std::size_t> parent(n);
        for (std::size_t i=0; i<n; ++i) { parent[i]=i; }
        const auto root = [&](std::size_t i)
        {
            while (parent[i] != i) { i = parent[i]; }
            return i;
        };
        const f64 threshold = std::cos(config_.neighbourRadians);
        // Bounded candidate pass. Later replace pair search with a spatial hash
        // when a global opportunity provider makes very large candidate sets.
        for (std::size_t i=0; i<n; ++i)
            for (std::size_t j=i+1; j<n; ++j)
                if (StormZoneDot(candidates[i].p.direction, candidates[j].p.direction) >= threshold)
                {
                    const auto a=root(i), b=root(j);
                    if (a!=b) { parent[b]=a; }
                }

        std::vector<StormZone> next;
        std::vector<bool> used(zones_.size(), false);
        for (std::size_t i=0; i<n; ++i)
        {
            if (root(i)!=i) { continue; }
            math::Double3 sum{0,0,0};
            f64 total=0;
            f64 initiation=0, convection=0, moisture=0, precip=0;
            bool active=false;
            u32 count=0;
            for (std::size_t j=0; j<n; ++j)
            {
                if (root(j)!=i) { continue; }
                const auto& p=candidates[j].p;
                const f64 w=std::max(0.01F,p.opportunity);
                sum.x+=p.direction.x*w; sum.y+=p.direction.y*w; sum.z+=p.direction.z*w;
                total+=w; initiation+=w*p.opportunity;
                convection+=w*StormZoneClamp(p.convection);
                moisture+=w*StormZoneClamp(p.moisture);
                precip+=w*StormZoneClamp(p.precipitation);
                active |= candidates[j].active;
                ++count;
            }
            if (!active || total<=0) { continue; } // no zone from weak probes alone
            StormZone zone;
            zone.centre=StormZoneUnit(sum);
            zone.contributingProbes=count;
            zone.weights.initiation=static_cast<f32>(initiation/total);
            zone.weights.convection=static_cast<f32>(convection/total);
            zone.weights.moisture=static_cast<f32>(moisture/total);
            zone.weights.precipitation=static_cast<f32>(precip/total);
            // Provisional organization proxies, NOT physical type predictions.
            zone.weights.multicell=zone.weights.convection * zone.weights.moisture;
            zone.weights.organized=zone.weights.convection;
            for (std::size_t j=0; j<n; ++j)
                if (root(j)==i)
                    zone.radiusRadians=std::max(zone.radiusRadians,
                        std::acos(StormZoneDot(zone.centre,candidates[j].p.direction)));
            std::size_t match=zones_.size();
            f64 closest=config_.matchRadians;
            for (std::size_t j=0; j<zones_.size(); ++j)
            {
                const f64 distance=std::acos(StormZoneDot(zone.centre,zones_[j].centre));
                if (!used[j] && distance<closest) { closest=distance; match=j; }
            }
            if (match<zones_.size())
            {
                used[match]=true;
                zone.id=zones_[match].id;
            }
            else { zone.id=nextId_++; }
            zone.lastSeenSeconds=seconds;
            next.push_back(zone);
        }
        // Stable dormant zones can outlive temporary dips in opportunity.
        for (std::size_t j=0; j<zones_.size(); ++j)
            if (!used[j] && seconds-zones_[j].lastSeenSeconds<=config_.retireAfterSeconds)
                next.push_back(zones_[j]);
        std::sort(next.begin(), next.end(), [](const StormZone& a,const StormZone& b)
        {
            if (a.contributingProbes!=b.contributingProbes)
                return a.contributingProbes>b.contributingProbes;
            return a.id<b.id;
        });
        if (next.size()>config_.maxZones) next.resize(config_.maxZones);
        zones_=std::move(next);
    }

    [[nodiscard]] const std::vector<StormZone>& Zones() const noexcept { return zones_; }
private:
    StormZoneConfig config_;
    std::vector<StormZone> zones_;
    u64 nextId_{1};
    f64 lastTime_{-std::numeric_limits<f64>::infinity()};
};
} // namespace orbit::celestial_clouds
