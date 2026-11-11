#include <orbit/path_geometry/JunctionConflicts.hpp>
#include <iostream>
#define CHECK(x) do {if (!(x)){std::cerr<<"FAIL " #x " at "<<__LINE__<<'\n';return 1;}} while(false)

int main()
{
    using namespace orbit::path_geometry;
    using orbit::scene::ObjectId;
    JunctionPlan preview;
    preview.node={1,1};
    preview.movements={
        {.incomingEdge={2,1},.outgoingEdge={2,2},
         .mode=TransportMode::Road,.points={{-10,0,0},{10,0,0}}},
        {.incomingEdge={2,3},.outgoingEdge={2,4},
         .mode=TransportMode::Road,.points={{0,0,-10},{0,0,10}}},
        {.incomingEdge={2,5},.outgoingEdge={2,6},
         .mode=TransportMode::Rail,.points={{-10,5,0},{10,5,0}}},
        {.incomingEdge={2,7},.outgoingEdge={2,8},
         .mode=TransportMode::Road,.points={{-10,0,20},{10,0,20}}}
    };
    const auto report=AnalyzeJunctionConflicts(preview);
    CHECK(report.valid);
    CHECK(!report.truncated);
    CHECK(report.evaluatedPairs==6);
    CHECK(report.conflicts.size()==1);
    CHECK(report.conflicts[0].firstMovement==0);
    CHECK(report.conflicts[0].secondMovement==1);
    CHECK(report.conflicts[0].kind==JunctionConflictKind::GeometricOverlap);
    const auto deterministic=AnalyzeJunctionConflicts(preview);
    CHECK(deterministic.conflicts.size()==report.conflicts.size());
    CHECK(deterministic.conflicts[0].firstMovement==report.conflicts[0].firstMovement);
    auto shared=preview;
    shared.movements.push_back({
        .incomingEdge={2,1},.outgoingEdge={2,10},
        .mode=TransportMode::Road,.points={{-10,0,0},{-10,0,50}}
    });
    const auto sharedResult=AnalyzeJunctionConflicts(shared);
    bool foundShared=false;
    for(const auto& conflict:sharedResult.conflicts)
        if(conflict.kind==JunctionConflictKind::SharedLane)foundShared=true;
    CHECK(foundShared);
    const auto limited=AnalyzeJunctionConflicts(preview,
        {.maximumEvaluatedPairs=1});
    CHECK(limited.valid && limited.truncated);
    CHECK(limited.evaluatedPairs==1);
    const auto limitedSegments=AnalyzeJunctionConflicts(preview,
        {.maximumSegmentComparisons=1});
    CHECK(limitedSegments.valid && limitedSegments.truncated);
    auto invalid=preview;
    invalid.movements[1].points.clear();
    CHECK(!AnalyzeJunctionConflicts(invalid).valid);
    return 0;
}
