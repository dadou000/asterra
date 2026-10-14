#include <orbit/path_geometry/RoadJunctionContact.hpp>
#include <cmath>
#include <iostream>
#include <stdexcept>
#define CHECK(x) do{if(!(x)){std::cerr<<"Road contact test: " #x " line "<<__LINE__<<'\n';return 1;}}while(false)
int main()
{
    using namespace orbit;
    using namespace orbit::path_geometry;
    const frames::FrameId frame{1,1};
    RoadJunctionScenePacket packet;
    packet.frame=frame;
    packet.worldGeneration=1;
    packet.buildSignature=22;
    RoadJunctionPatchProduct patch;
    patch.node={2,1};
    patch.frame=frame;
    patch.dependencySignature=12;
    patch.mesh.vertices={
        {.position={-5,0,-5}},{.position={5,0,-5}},
        {.position={5,0,5}},{.position={-5,0,5}}
    };
    patch.mesh.indices={0,1,2,0,2,3};
    patch.collision={
        {{-5,0,-5},{5,0,-5},{5,0,5}},
        {{-5,0,-5},{5,0,5},{-5,0,5}}
    };
    packet.patches.push_back(patch);
    // A separate intact section outside the junction, to verify owner.
    RoadJunctionPublishedEdge edge;
    edge.edge={3,1};
    edge.product.edge=edge.edge;
    edge.product.frame=frame;
    edge.product.collision={
        {{10,0,-5},{20,0,-5},{20,0,5}},
        {{10,0,-5},{20,0,5},{10,0,5}}
    };
    packet.edges.push_back(edge);
    auto hit=ProbeRoadJunctionContact(packet,{
        .frame=frame,.origin={1,10,2},.direction={0,-1,0}});
    CHECK(hit.hit);
    CHECK(!hit.budgetExceeded);
    CHECK(hit.junctionPatch);
    CHECK(hit.owner==patch.node);
    CHECK(std::abs(hit.distanceMeters-10.0)<1.e-9);
    CHECK(std::abs(hit.normal.y-1.0)<1.e-9);
    hit=ProbeRoadJunctionContact(packet,{
        .frame=frame,.origin={15,10,0},.direction={0,-2,0}});
    CHECK(hit.hit && !hit.junctionPatch && hit.owner==edge.edge);
    CHECK(std::abs(hit.distanceMeters-10.0)<1.e-9);
    const auto miss=ProbeRoadJunctionContact(packet,{
        .frame=frame,.origin={0,10,20},.direction={0,-1,0}});
    CHECK(!miss.hit && !miss.budgetExceeded);
    const auto bounded=ProbeRoadJunctionContact(packet,{
        .frame=frame,.origin={1,10,2},.direction={0,-1,0},
        .maximumTriangles=1U});
    CHECK(bounded.budgetExceeded);
    bool rejected=false;
    try {static_cast<void>(ProbeRoadJunctionContact(packet,{
        .frame={8,8},.origin={1,10,2}}));}
    catch(const std::invalid_argument&){rejected=true;}
    CHECK(rejected);
    rejected=false;
    try {static_cast<void>(ProbeRoadJunctionContact(packet,{
        .frame=frame,.origin={1,10,2},.direction={0,0,0}}));}
    catch(const std::invalid_argument&){rejected=true;}
    CHECK(rejected);
    return 0;
}
