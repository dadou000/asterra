#include <orbit/path_geometry/JunctionSignalPhases.hpp>
#include <algorithm>
#include <numeric>

namespace orbit::path_geometry
{
JunctionPhasePlan SuggestRoadJunctionPhases(
    const JunctionPlan& plan, const JunctionConflictReport& conflicts,
    const u32 maximumPhases)
{
    JunctionPhasePlan result;
    result.requiresRailInterlocking=plan.hasRailApproach;
    for(const auto& m:plan.movements)
        if(m.mode!=TransportMode::Road)
            result.requiresRailInterlocking=true;
    const std::size_t n=plan.movements.size();
    if(!plan.Valid() || !conflicts.valid || conflicts.truncated ||
       result.requiresRailInterlocking || n==0U || n>256U ||
       maximumPhases==0U || maximumPhases>256U)
        return result;
    // A small bounded adjacency matrix makes validation and replays trivial.
    std::vector<std::vector<bool>> adjacent(n,std::vector<bool>(n,false));
    std::vector<u32> degree(n,0);
    for(const auto& c:conflicts.conflicts)
    {
        if(c.firstMovement>=n || c.secondMovement>=n ||
           c.firstMovement==c.secondMovement)return result;
        if(!adjacent[c.firstMovement][c.secondMovement])
        {
            adjacent[c.firstMovement][c.secondMovement]=true;
            adjacent[c.secondMovement][c.firstMovement]=true;
            ++degree[c.firstMovement];++degree[c.secondMovement];
        }
    }
    std::vector<u32> order(n);
    std::iota(order.begin(),order.end(),0U);
    std::stable_sort(order.begin(),order.end(),
        [&](u32 a,u32 b) {
            return degree[a]!=degree[b]?degree[a]>degree[b]:a<b;
        });
    for(const u32 movement:order)
    {
        bool assigned=false;
        for(auto& phase:result.phases)
        {
            const bool compatible=std::all_of(
                phase.movementIndices.begin(),phase.movementIndices.end(),
                [&](u32 other){return !adjacent[movement][other];});
            if(compatible)
            {
                phase.movementIndices.push_back(movement);
                assigned=true;
                break;
            }
        }
        if(assigned)continue;
        if(result.phases.size()>=maximumPhases)
        {
            result.phases.clear();
            return result;
        }
        result.phases.push_back({{movement}});
    }
    for(auto& phase:result.phases)
        std::sort(phase.movementIndices.begin(),phase.movementIndices.end());
    result.valid=true;
    return result;
}
} // namespace orbit::path_geometry
