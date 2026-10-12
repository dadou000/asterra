#pragma once

#include <orbit/celestial_clouds/WeatherConvectiveInteractions.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <vector>

namespace orbit::celestial_clouds
{
// Phase 7 experimental *organization* operator, not a meteorological CFD solver.
// Cold-pool strength and radius are normalized placeholders; no physical
// boundary-layer temperature, momentum, or mass source is synthesized.
struct WeatherEmergenceConfig
{
    f64 maxStepSeconds{120.0};
    f64 coldPoolGrowthKmPerSecond{0.002};
    f64 coldPoolDissipationPerSecond{0.00012};
    f64 coldPoolProductionPerSecond{0.0009};
    f64 propagationStrength{0.00007};
    f64 undercutStrength{0.00016};
    f64 coherentDwellSeconds{240.0};
    f64 minOrganizedIntensity{0.10};
    f64 maxTravelKmPerSecond{0.035};
};

// A group is formed only by strong, observed graph connectivity for a
// sustained interval. It never assigns a supercell / squall line label.
class WeatherEmergence
{
public:
    [[nodiscard]] static bool Advance(WeatherZoneEvolutionState& state,
        f64 dt, WeatherEmergenceConfig config={})
    {
        if (!std::isfinite(dt) || dt<=0 || dt>config.maxStepSeconds)
            return false;
        const auto previous=state.children;
        for (std::size_t i=0;i<previous.size();++i)
        {
            const auto& input=previous[i];
            if (!std::isfinite(input.eastKm) || !std::isfinite(input.northKm) ||
                !std::isfinite(input.eastKmPerSecond) ||
                !std::isfinite(input.northKmPerSecond) ||
                !std::isfinite(input.coldPoolKm) ||
                !std::isfinite(input.coldPoolStrength) ||
                !std::isfinite(input.updraft) ||
                !std::isfinite(input.rain) ||
                input.coldPoolKm<0 || input.coldPoolStrength<0 ||
                input.rain<0)
                return false;
        }
        // Use old state for every interaction, avoiding update-order bias.
        for (std::size_t i=0;i<previous.size();++i)
        {
            const auto& old=previous[i];
            auto& c=state.children[i];
            const f64 u=std::clamp(old.eastKmPerSecond,
                -config.maxTravelKmPerSecond,config.maxTravelKmPerSecond);
            const f64 v=std::clamp(old.northKmPerSecond,
                -config.maxTravelKmPerSecond,config.maxTravelKmPerSecond);
            c.eastKm=old.eastKm+u*dt;
            c.northKm=old.northKm+v*dt;
            const f64 production=(1.0-std::exp(-config.coldPoolProductionPerSecond*dt)) *
                std::clamp(old.rain,0.0,1.0);
            c.coldPoolStrength=std::clamp(old.coldPoolStrength *
                std::exp(-config.coldPoolDissipationPerSecond*dt)+production,0.0,1.0);
            c.coldPoolKm=std::clamp(old.coldPoolKm+
                config.coldPoolGrowthKmPerSecond*dt*c.coldPoolStrength,0.0,80.0);
        }
        // Cold-pool feedback changes intensity only, not conserved inventory.
        // At the expanding edge: constructive lift. Deep inside: undercut.
        for (std::size_t i=0;i<state.children.size();++i)
        {
            f64 intensity=state.children[i].updraft;
            for (std::size_t j=0;j<previous.size();++j)
            {
                if(i==j) continue;
                const auto& pool=state.children[j];
                if (pool.coldPoolKm<0.001 || pool.coldPoolStrength<0.001) continue;
                const f64 distance=std::hypot(state.children[i].eastKm-pool.eastKm,
                    state.children[i].northKm-pool.northKm);
                const f64 shell=std::max(2.0,0.15*pool.coldPoolKm);
                const f64 edge=std::exp(-std::pow((distance-pool.coldPoolKm)/shell,2.0));
                const f64 inside=distance<pool.coldPoolKm*0.65 ? 1.0 : 0.0;
                intensity+=dt*pool.coldPoolStrength *
                    (config.propagationStrength*edge-config.undercutStrength*inside);
            }
            state.children[i].updraft=std::clamp(intensity,0.0,1.0);
        }

        const auto graph=WeatherConvectiveInteractions::Build(state);
        std::vector<WeatherZoneEvolutionState::Group> groups;
        for (const auto& island:graph.islands)
        {
            if (island.children.size()<2) continue;
            f64 totalIntensity=0.0;
            for (u64 member:island.children)
                for (const auto& c:state.children)
                    if(c.id==member) totalIntensity+=c.updraft;
            if(totalIntensity/static_cast<f64>(island.children.size()) <
                config.minOrganizedIntensity) continue;

            WeatherZoneEvolutionState::Group group;
            group.members=island.children;
            // Persist the identity only for exact membership. A merger or
            // split creates a new organization history, not forced continuity.
            const auto match=std::find_if(state.groups.begin(),state.groups.end(),
                [&](const auto& g){return g.members==group.members;});
            if(match!=state.groups.end())
            {
                group.id=match->id;
                group.coherentSeconds=match->coherentSeconds+dt;
            }
            else
            {
                // Stable identity derived from member IDs and zone ID.
                u64 hash=1469598103934665603ULL ^ state.zoneId;
                for (u64 member:group.members)
                {
                    hash^=member;
                    hash*=1099511628211ULL;
                }
                group.id=hash==0?1:hash;
                group.coherentSeconds=dt;
            }
            // Short-lived transient connectivity isn't organized weather.
            groups.push_back(std::move(group));
        }
        state.groups=std::move(groups);
        return true;
    }

    [[nodiscard]] static u32 OrganizedGroups(const WeatherZoneEvolutionState& state,
        f64 dwellSeconds=240.0)
    {
        u32 count=0;
        for (const auto& group:state.groups)
            if(group.coherentSeconds>=dwellSeconds) ++count;
        return count;
    }
};

// Fits inside existing transactional execution: first conservative local
// evolution, then pair exchanges, then diagnostic organization and motion.
[[nodiscard]] inline std::optional<WeatherEvolutionResult> EvaluateEmergentWeather(
    const WeatherZoneEvolutionState& original,
    const StormZoneWeights& environment,
    const WeatherStepTicket& ticket)
{
    auto result=EvaluateInteractingWeather(original,environment,ticket);
    if (!result) return std::nullopt;
    if (!WeatherEmergence::Advance(result->state,
        ticket.toSeconds-ticket.fromSeconds)) return std::nullopt;
    return result;
}
} // namespace orbit::celestial_clouds
