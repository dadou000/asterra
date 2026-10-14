#include <orbit/path_geometry/JunctionSignalPhases.hpp>
#include <iostream>
#define CHECK(x) do {if (!(x)){std::cerr<<"FAIL " #x " line "<<__LINE__<<'\n';return 1;}} while(false)

int main()
{
    using namespace orbit::path_geometry;
    JunctionPlan road;
    road.node={1,1};
    for(int i=0;i<4;++i)
        road.movements.push_back({
            .incomingEdge={2,static_cast<orbit::u64>(i+1)},
            .outgoingEdge={3,static_cast<orbit::u64>(i+1)},
            .mode=TransportMode::Road,
            .points={{static_cast<double>(i),0,0},
                     {static_cast<double>(i),0,10}}
        });
    JunctionConflictReport conflicts;
    conflicts.valid=true;
    conflicts.conflicts={
        {.firstMovement=0,.secondMovement=1},
        {.firstMovement=0,.secondMovement=2},
        {.firstMovement=2,.secondMovement=3}
    };
    const auto plan=SuggestRoadJunctionPhases(road,conflicts);
    CHECK(plan.valid);
    CHECK(plan.phases.size()>=2);
    bool assigned[4]{};
    for(const auto& phase:plan.phases)
    {
        for(auto i:phase.movementIndices)
        {
            CHECK(i<4);
            CHECK(!assigned[i]);
            assigned[i]=true;
        }
        for(const auto& conflict:conflicts.conflicts)
        {
            bool a=false,b=false;
            for(auto i:phase.movementIndices)
            {
                a|=i==conflict.firstMovement;
                b|=i==conflict.secondMovement;
            }
            CHECK(!(a&&b));
        }
    }
    for(bool value:assigned)CHECK(value);
    const auto repeated=SuggestRoadJunctionPhases(road,conflicts);
    CHECK(repeated.valid);
    CHECK(repeated.phases.size()==plan.phases.size());
    CHECK(repeated.phases.front().movementIndices==
          plan.phases.front().movementIndices);
    CHECK(!SuggestRoadJunctionPhases(road,conflicts,1).valid);
    conflicts.truncated=true;
    CHECK(!SuggestRoadJunctionPhases(road,conflicts).valid);
    conflicts.truncated=false;
    road.movements[3].mode=TransportMode::Rail;
    const auto mixed=SuggestRoadJunctionPhases(road,conflicts);
    CHECK(!mixed.valid && mixed.requiresRailInterlocking);
    return 0;
}
