#pragma once

#include <orbit/celestial_clouds/WeatherEvolution.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <utility>
#include <vector>

namespace orbit::celestial_clouds
{
// Phase 6: local sparse interactions for the EXPERIMENTAL normalized surrogate.
// Positions are local-zone kilometres, not global coordinates. Interaction
// coefficients are placeholders, not CM1-calibrated entrainment/cold-pool physics.
struct ConvectiveInteractionEdge
{
    u64 first{0};
    u64 second{0};
    f64 separationKm{0.0};
    f64 influence{0.0};
};
struct ConvectiveCouplingIsland
{
    std::vector<u64> children;
};
struct ConvectiveInteractionGraph
{
    std::vector<ConvectiveInteractionEdge> edges;
    std::vector<ConvectiveCouplingIsland> islands;
};
struct ConvectiveInteractionConfig
{
    f64 influenceRadiusKm{35.0};
    f64 couplingThreshold{0.25};
    f64 moistureExchangeRatePerSecond{0.0002};
    f64 updraftCompetitionPerSecond{0.0001};
    f64 liftingPerSecond{0.00008};
    std::size_t maxChildren{64};
    std::size_t maxEdges{512};
};

class WeatherConvectiveInteractions
{
public:
    [[nodiscard]] static ConvectiveInteractionGraph Build(
        const WeatherZoneEvolutionState& state,
        ConvectiveInteractionConfig config={})
    {
        ConvectiveInteractionGraph graph;
        // Stable sorting is necessary for reproducible pair evaluation, even
        // if a caller supplies children in a different order.
        std::vector<const ConvectiveChildState*> children;
        children.reserve(state.children.size());
        for (const auto& child:state.children) children.push_back(&child);
        std::sort(children.begin(),children.end(),[](const auto* a,const auto* b)
        {return a->id<b->id;});
        if (children.size()>config.maxChildren) return graph;
        for (std::size_t i=1;i<children.size();++i)
            if(children[i-1]->id==children[i]->id) return graph;
        const f64 radius=std::max(0.001,config.influenceRadiusKm);
        for (std::size_t i=0;i<children.size();++i)
            for (std::size_t j=i+1;j<children.size();++j)
            {
                const auto& a=*children[i];
                const auto& b=*children[j];
                const f64 distance=std::hypot(a.eastKm-b.eastKm,a.northKm-b.northKm);
                if (!std::isfinite(distance) || distance>=radius) continue;
                const f64 x=1.0-distance/radius;
                const f64 influence=x*x*(3.0-2.0*x);
                graph.edges.push_back({a.id,b.id,distance,influence});
                if(graph.edges.size()>=config.maxEdges) break;
            }
        // Sparse connectivity graph: components share a synchronization island
        // only when the coupling is sufficiently strong.
        std::vector<std::size_t> parent(children.size());
        std::iota(parent.begin(),parent.end(),0);
        const auto root=[&parent](std::size_t index)
        {
            while(parent[index]!=index) index=parent[index];
            return index;
        };
        for(const auto& edge:graph.edges)
        {
            if(edge.influence<config.couplingThreshold) continue;
            const auto a=std::lower_bound(children.begin(),children.end(),edge.first,
                [](const auto* p,u64 id){return p->id<id;});
            const auto b=std::lower_bound(children.begin(),children.end(),edge.second,
                [](const auto* p,u64 id){return p->id<id;});
            const auto ra=root(static_cast<std::size_t>(a-children.begin()));
            const auto rb=root(static_cast<std::size_t>(b-children.begin()));
            if(ra!=rb) parent[rb]=ra;
        }
        for(std::size_t i=0;i<children.size();++i)
        {
            const auto r=root(i);
            if(r==i) continue;
            parent[i]=r;
        }
        // Deterministic islands in lowest-id order, including isolated cells.
        for(std::size_t i=0;i<children.size();++i)
        {
            if(parent[i]!=i) continue;
            ConvectiveCouplingIsland island;
            for(std::size_t j=0;j<children.size();++j)
                if(root(j)==i) island.children.push_back(children[j]->id);
            graph.islands.push_back(std::move(island));
        }
        return graph;
    }

    // Apply edges from an immutable pre-interaction snapshot: conservative
    // vapor redistribution and nonconservative *diagnostic only* intensity
    // response. No heat/momentum/latent-water creation is implied.
    [[nodiscard]] static bool Apply(
        WeatherZoneEvolutionState& state,
        const ConvectiveInteractionGraph& graph,
        f64 dt,
        ConvectiveInteractionConfig config={})
    {
        if(!std::isfinite(dt) || dt<0 || dt>120 ||
            state.children.size()>config.maxChildren ||
            graph.edges.size()>config.maxEdges) return false;
        std::vector<std::size_t> order(state.children.size());
        std::iota(order.begin(),order.end(),0);
        std::sort(order.begin(),order.end(),[&](std::size_t a,std::size_t b)
        {return state.children[a].id<state.children[b].id;});
        std::vector<f64> vapourDelta(state.children.size(),0.0);
        std::vector<f64> intensityDelta(state.children.size(),0.0);
        const auto snapshot=state.children;
        for(const auto& e:graph.edges)
        {
            const auto findIndex=[&](u64 id)->std::size_t
            {
                const auto it=std::lower_bound(order.begin(),order.end(),id,
                    [&](std::size_t index,u64 key){return snapshot[index].id<key;});
                return it!=order.end() && snapshot[*it].id==id ? *it : snapshot.size();
            };
            const auto a=findIndex(e.first),b=findIndex(e.second);
            if(a==snapshot.size() || b==snapshot.size() || a==b ||
                !std::isfinite(e.influence) || e.influence<0 || e.influence>1) return false;
            const f64 strength=e.influence*dt;
            const f64 diff=snapshot[a].vapour-snapshot[b].vapour;
            const f64 desired=diff*std::min(0.49,config.moistureExchangeRatePerSecond*strength);
            vapourDelta[a]-=desired;
            vapourDelta[b]+=desired;
            // Strong shared outflow competes for inflow; moderate boundaries
            // can locally increase lifting. These only modify the diagnostic.
            const f64 competition=std::min(snapshot[a].updraft,snapshot[b].updraft);
            const f64 boost=config.liftingPerSecond*strength*(1.0-e.influence);
            const f64 loss=config.updraftCompetitionPerSecond*strength*competition;
            intensityDelta[a]+=boost-loss;
            intensityDelta[b]+=boost-loss;
        }
        // A child can participate in multiple edges. Scale *all* transfers
        // globally if required, maintaining symmetric conservation.
        f64 scale=1.0;
        for(std::size_t i=0;i<snapshot.size();++i)
        {
            if(!std::isfinite(snapshot[i].vapour) || snapshot[i].vapour<0 ||
                !std::isfinite(snapshot[i].updraft)) return false;
            if(vapourDelta[i]<0)
                scale=std::min(scale,snapshot[i].vapour/(-vapourDelta[i]));
        }
        for(std::size_t i=0;i<state.children.size();++i)
        {
            state.children[i].vapour=std::max(0.0,snapshot[i].vapour+scale*vapourDelta[i]);
            state.children[i].updraft=std::clamp(snapshot[i].updraft+intensityDelta[i],0.0,1.0);
        }
        return true;
    }
};
} // namespace orbit::celestial_clouds
