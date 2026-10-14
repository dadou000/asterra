#include <orbit/path_geometry/RoadVehicleWorld.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>
#define CHECK(x) do{if(!(x)){std::cerr<<"RoadVehicleWorld: " #x " at "<<__LINE__<<'\n';return 1;}}while(false)

namespace {
using namespace orbit;
using namespace orbit::path_geometry;
void Add(RoadJunctionScenePacket& packet,scene::ObjectId id,
         f64 low,f64 high)
{
    RoadJunctionPublishedEdge road;
    road.edge=id;
    road.rawBuildSignature=20;
    road.product.edge=id;
    road.product.frame=packet.frame;
    road.product.visualMesh.vertices={
        {.position={low,0,-4}},{.position={high,0,-4}},
        {.position={high,0,4}},{.position={low,0,4}}
    };
    road.product.visualMesh.indices={0,1,2,0,2,3};
    road.product.collision={
        {{low,0,-4},{high,0,-4},{high,0,4}},
        {{low,0,-4},{high,0,4},{low,0,4}}
    };
    LaneReference lane;
    lane.laneIndex=0;
    lane.points={{.position={low+1,0,0}},
                 {.position={high-1,0,0}}};
    road.product.lanes.push_back(lane);
    packet.edges.push_back(std::move(road));
}
void Patch(RoadJunctionScenePacket& p)
{
    RoadJunctionPatchProduct patch;
    patch.node={9,1};
    patch.frame=p.frame;
    patch.dependencySignature=40;
    patch.mesh.vertices={
        {.position={-2,0,-4}},{.position={2,0,-4}},
        {.position={2,0,4}},{.position={-2,0,4}}
    };
    patch.mesh.indices={0,1,2,0,2,3};
    patch.collision={
        {{-2,0,-4},{2,0,-4},{2,0,4}},
        {{-2,0,-4},{2,0,4},{-2,0,4}}
    };
    p.patches.push_back(std::move(patch));
}
}
int main()
{
    using namespace orbit;
    using namespace orbit::path_geometry;
    RoadJunctionScenePacket packet;
    packet.frame={1,1};
    packet.worldGeneration=5;
    packet.buildSignature=20;
    Add(packet,{2,1},-25,-2);
    Add(packet,{2,2},2,25);
    Patch(packet);
    packet.connections.push_back({
        .incomingEdge={2,1},.outgoingEdge={2,2},
        .incomingLane=0,.outgoingLane=0,
        .mode=TransportMode::Road,
        .points={{-3,0,0},{0,0,0},{3,0,0}},.lengthMeters=6
    });
    // Source road station ends at -3 and destination begins at +3;
    // therefore the connector is an exact authored movement.
    const auto route=CompileRoadVehicleRoute(packet,{2,1},{2,2});
    CHECK(route.has_value());
    CHECK(route->Valid());
    CHECK(route->waypoints.size()>=5U);
    CHECK(!CompileRoadVehicleRoute(packet,{2,2},{2,1}).has_value());
    RoadContactSceneOwner contact;
    auto cooked=PrepareRoadContactScene(packet);
    CHECK(cooked.Ready());
    CHECK(contact.Commit(std::move(cooked)));
    RoadVehicleRuntimeWorld first, second;
    CHECK(first.Bind(contact));
    CHECK(second.Bind(contact));
    RoadVehicleState start;
    start.centre={-9,0.69,0};
    start.forwardSpeedMetersPerSecond=4.0;
    const scene::ObjectId id{12,1};
    CHECK(first.Spawn(contact,id,start));
    CHECK(second.Spawn(contact,id,start));
    CHECK(!first.Spawn(contact,id,start));
    RoadVehicleState invalid=start;
    invalid.centre.x=std::numeric_limits<double>::quiet_NaN();
    CHECK(!first.Spawn(contact,{12,2},invalid));
    CHECK(first.SetControls(id,{.throttle=0.14}));
    CHECK(second.SetControls(id,{.throttle=0.14}));
    const auto a=first.Step(contact,120U);
    const auto b=second.Step(contact,120U);
    CHECK(a.success && b.success);
    CHECK(first.Tick()==120U);
    CHECK(first.Find(id)!=nullptr);
    CHECK(first.Find(id)->state.centre==second.Find(id)->state.centre);
    CHECK(first.Find(id)->state.centre.x>start.centre.x);
    CHECK(!first.Step(contact,0).success);
    CHECK(!first.Step(contact,121).success);
    CHECK(first.Tick()==120U);
    CHECK(first.FollowRoute(id,*route,4.0));
    CHECK(first.Step(contact,120).success);
    CHECK(first.Find(id)->followRoute);
    CHECK(first.StopFollowing(id));
    CHECK(!first.Find(id)->followRoute);
    CHECK(first.Remove(id));
    CHECK(first.Find(id)==nullptr);
    CHECK(!first.Remove(id));
    const auto oldState=second.Find(id)->state;
    packet.buildSignature=21;
    auto next=PrepareRoadContactScene(packet);
    CHECK(next.Ready());
    CHECK(contact.Commit(std::move(next)));
    CHECK(!second.Step(contact,1).success);
    CHECK(second.Find(id)->state.centre==oldState.centre);
    CHECK(second.Bind(contact));
    CHECK(second.Count()==0U);
    CHECK(second.Tick()==0U);
    CHECK(!second.FollowRoute(id,*route));
    CHECK(second.Spawn(contact,id,start));
    second.Clear();
    CHECK(second.Count()==0U);
    CHECK(!second.Step(contact,1).success);
    return 0;
}
