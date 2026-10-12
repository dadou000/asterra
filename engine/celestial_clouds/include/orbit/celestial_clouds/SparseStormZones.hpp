#pragma once

#include <orbit/celestial_clouds/WeatherModel.hpp>
#include <orbit/core/Types.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>
#include <utility>
#include <unordered_map>

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
        // Unit-sphere Cartesian hash. Adjacent probes closer than the angular
        // threshold must occupy the same or neighboring cube buckets.
        // Expected near-linear candidate clustering; no global dense allocation.
        struct Key
        {
            i32 x, y, z;
            bool operator==(const Key&) const = default;
        };
        struct KeyHash
        {
            [[nodiscard]] std::size_t operator()(const Key& k) const noexcept
            {
                u64 h = 1469598103934665603ULL;
                for (const i32 v : {k.x, k.y, k.z})
                {
                    h ^= static_cast<u64>(static_cast<i64>(v));
                    h *= 1099511628211ULL;
                }
                return static_cast<std::size_t>(h);
            }
        };
        const f64 angle = std::clamp(config_.neighbourRadians, 1.0e-6, 3.141592653589793);
        const f64 chord = 2.0 * std::sin(angle * 0.5);
        const f64 threshold = std::cos(angle);
        const auto keyFor = [chord](const math::Double3& p) -> Key
        {
            return {static_cast<i32>(std::floor((p.x + 1.0) / chord)),
                    static_cast<i32>(std::floor((p.y + 1.0) / chord)),
                    static_cast<i32>(std::floor((p.z + 1.0) / chord))};
        };
        std::unordered_map<Key, std::vector<std::size_t>, KeyHash> buckets;
        buckets.reserve(n);
        for (std::size_t i=0; i<n; ++i)
        {
            const Key k = keyFor(candidates[i].p.direction);
            for (i32 x=-1; x<=1; ++x)
                for (i32 y=-1; y<=1; ++y)
                    for (i32 z=-1; z<=1; ++z)
                    {
                        const auto found=buckets.find({k.x+x,k.y+y,k.z+z});
                        if (found==buckets.end()) { continue; }
                        for (const auto j : found->second)
                            if (StormZoneDot(candidates[i].p.direction,
                                             candidates[j].p.direction)>=threshold)
                            {
                                const auto a=root(i), b=root(j);
                                if (a!=b) { parent[b]=a; }
                            }
                    }
            buckets[k].push_back(i);
        }
        // Compress all roots once, then accumulate each group in one pass.
        for (std::size_t i=0; i<n; ++i) { parent[i]=root(i); }

        std::vector<StormZone> next;
        std::vector<bool> used(zones_.size(), false);
        struct Accumulator
        {
            math::Double3 sum{0,0,0};
            f64 total{0}, initiation{0}, convection{0}, moisture{0}, precipitation{0};
            u32 count{0};
            bool active{false};
        };
        std::vector<Accumulator> aggregates(n);
        for (std::size_t i=0; i<n; ++i)
        {
            auto& a=aggregates[parent[i]];
            const auto& p=candidates[i].p;
            const f64 w=std::max(0.01F,p.opportunity);
            a.sum.x+=p.direction.x*w;
            a.sum.y+=p.direction.y*w;
            a.sum.z+=p.direction.z*w;
            a.total+=w;
            a.initiation+=w*p.opportunity;
            a.convection+=w*StormZoneClamp(p.convection);
            a.moisture+=w*StormZoneClamp(p.moisture);
            a.precipitation+=w*StormZoneClamp(p.precipitation);
            a.active |= candidates[i].active;
            ++a.count;
        }
        // First determine representative centres, then radii in a separate pass.
        std::vector<StormZone> produced;
        std::vector<std::size_t> groupToZone(n,n);
        for (std::size_t i=0; i<n; ++i)
        {
            const auto& a=aggregates[i];
            if (!a.active || a.total<=0) { continue; }
            StormZone zone;
            zone.centre=StormZoneUnit(a.sum);
            zone.contributingProbes=a.count;
            zone.weights.initiation=static_cast<f32>(a.initiation/a.total);
            zone.weights.convection=static_cast<f32>(a.convection/a.total);
            zone.weights.moisture=static_cast<f32>(a.moisture/a.total);
            zone.weights.precipitation=static_cast<f32>(a.precipitation/a.total);
            // Proxies, not physical storm-class scores.
            zone.weights.multicell=zone.weights.convection*zone.weights.moisture;
            zone.weights.organized=zone.weights.convection;
            groupToZone[i]=produced.size();
            produced.push_back(zone);
        }
        for (std::size_t i=0; i<n; ++i)
        {
            const auto index=groupToZone[parent[i]];
            if (index==n) { continue; }
            auto& zone=produced[index];
            zone.radiusRadians=std::max(zone.radiusRadians,
                std::acos(StormZoneDot(zone.centre,candidates[i].p.direction)));
        }
        for (auto& zone : produced)
        {
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
