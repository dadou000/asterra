#pragma once

#include <orbit/celestial_clouds/SparseStormZones.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

namespace orbit::celestial_clouds
{
// Read-only, camera-independent planetary survey. One analytic sample per
// equal-area Fibonacci point; only promising probes are returned. Intended as
// an interim source until the SIMD global atmospheric solver exposes candidates.
// No dense per-cell suitability or derived-weight arrays are created.
struct AnalyticZoneSurveyConfig
{
    u32 samples{1024};
    u32 maxCandidates{512};
    f32 minimumOpportunity{0.40F};
};

[[nodiscard]] inline std::vector<StormZoneProbe> SurveyAnalyticStormZones(
    f64 seconds,
    const WeatherParameters& weather,
    const WeatherClimate& climate,
    AnalyticZoneSurveyConfig config = {})
{
    std::vector<StormZoneProbe> probes;
    if (!std::isfinite(seconds) || config.maxCandidates==0 || config.samples==0) { return probes; }
    const u32 count=std::min(config.samples, 16384U);
    const u32 cap=std::min(config.maxCandidates, count);
    probes.reserve(cap);
    constexpr f64 goldenAngle=std::numbers::pi_v<f64> * (3.0-2.2360679774997896964);
    for (u32 i=0; i<count; ++i)
    {
        const f64 y=1.0-2.0*(static_cast<f64>(i)+0.5)/static_cast<f64>(count);
        const f64 r=std::sqrt(std::max(0.0,1.0-y*y));
        const f64 angle=goldenAngle*static_cast<f64>(i);
        const math::Double3 direction{r*std::cos(angle),y,r*std::sin(angle)};
        auto probe=AnalyticStormProbe(direction,seconds,weather,climate);
        if (probe.opportunity<config.minimumOpportunity) { continue; }
        // Keep the highest scoring candidates. This is an explicit O(N*K)
        // bounded scan for the diagnostic tool; no huge temporary planetary map.
        if (probes.size()<cap) { probes.push_back(probe); continue; }
        const auto lowest=std::min_element(probes.begin(),probes.end(),
            [](const StormZoneProbe& a,const StormZoneProbe& b)
            { return a.opportunity<b.opportunity; });
        if (probe.opportunity>lowest->opportunity) { *lowest=probe; }
    }
    // Deterministic ordering for stable budget truncation and comparisons.
    std::sort(probes.begin(),probes.end(),[](const auto& a,const auto& b)
    {
        if (a.opportunity!=b.opportunity) return a.opportunity>b.opportunity;
        if (a.direction.y!=b.direction.y) return a.direction.y>b.direction.y;
        return a.direction.x>b.direction.x;
    });
    return probes;
}
} // namespace orbit::celestial_clouds
