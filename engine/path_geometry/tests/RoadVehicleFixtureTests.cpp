#include <orbit/path_geometry/RoadVehicleFixture.hpp>

#include <cmath>
#include <iostream>

#define CHECK(e) do {if(!(e)){std::cerr<<"RoadVehicleFixture: " #e " at "<<__LINE__<<'\n';return 1;}}while(false)
namespace {
using namespace orbit;
using namespace orbit::path_geometry;
void AddSurface(RoadJunctionScenePacket& packet,
                scene::ObjectId owner,const bool patch,
                const f64 minX,const f64 maxX)
{
    PathMesh mesh;
    mesh.vertices={
        {.position={minX,0,-5}}, {.position={maxX,0,-5}},
        {.position={maxX,0,5}}, {.position={minX,0,5}}
    };
    mesh.indices={0,1,2,0,2,3};
    std::vector<CollisionTriangle> triangles{
        {{minX,0,-5},{maxX,0,-5},{maxX,0,5}},
        {{minX,0,-5},{maxX,0,5},{minX,0,5}}
    };
    if(patch)packet.patches.push_back({
        .node=owner,.frame=packet.frame,.dependencySignature=packet.buildSignature,
        .mesh=std::move(mesh),.collision=std::move(triangles)
    });
    else
    {
        RoadJunctionPublishedEdge road;
        road.edge=owner;
        road.rawBuildSignature=packet.buildSignature;
        road.product.edge=owner;
        road.product.frame=packet.frame;
        road.product.visualMesh=std::move(mesh);
        road.product.collision=std::move(triangles);
        packet.edges.push_back(std::move(road));
    }
}
}
int main()
{
    using namespace orbit;
    using namespace orbit::path_geometry;
    const frames::FrameId frame{5,1};
    RoadJunctionScenePacket packet;
    packet.frame=frame;
    packet.worldGeneration=3;
    packet.buildSignature=12;
    AddSurface(packet,{2,1},false,-25,-3);
    AddSurface(packet,{2,2},false,3,25);
    AddSurface(packet,{9,1},true,-3,3);
    RoadContactSceneOwner contact;
    auto built=PrepareRoadContactScene(packet);
    CHECK(built.Ready());
    CHECK(contact.Commit(std::move(built)));
    RoadVehicleFixtureRequest fixture;
    fixture.frame=frame;
    fixture.initial.centre={-9,0.69,0};
    fixture.initial.forwardSpeedMetersPerSecond=4.0;
    fixture.control.throttle=0.12;
    fixture.steps=480;
    const auto result=RunRoadVehicleFixture(contact,fixture);
    CHECK(result.completed);
    CHECK(result.error.empty());
    CHECK(result.simulatedSteps==480U);
    CHECK(result.stepsWithFullContact==480U);
    CHECK(result.stepsWithPatchContact>0U);
    CHECK(result.stepsWithRoadContact>0U);
    CHECK(result.wheelOwnerTransitions>=8U);
    CHECK(result.finalState.centre.x>5.0);
    CHECK(result.finalState.centre.x<25.0);
    CHECK(result.maximumBottomOutMeters<0.015);
    CHECK(result.maximumAbsPitchRadians<0.02);
    CHECK(result.maximumAbsRollRadians<0.02);
    CHECK(result.contactPacketSignature==12);
    CHECK(result.worldGeneration==3);
    CHECK(result.pathDistanceMeters>12.0);
    const auto repeat=RunRoadVehicleFixture(contact,fixture);
    CHECK(repeat.completed);
    CHECK(repeat.finalState.centre==result.finalState.centre);
    CHECK(repeat.wheelOwnerTransitions==result.wheelOwnerTransitions);
    CHECK(repeat.maximumBottomOutMeters==result.maximumBottomOutMeters);
    CHECK(repeat.stepsWithFullContact==result.stepsWithFullContact);

    RoadVehicleFixtureRequest stopped=fixture;
    stopped.control.throttle=0.0;
    stopped.control.brake=1.0;
    stopped.steps=120U;
    const auto braking=RunRoadVehicleFixture(contact,stopped);
    CHECK(braking.completed);
    CHECK(braking.finalState.forwardSpeedMetersPerSecond<
          fixture.initial.forwardSpeedMetersPerSecond);

    RoadVehicleFixtureRequest bad=fixture;
    bad.fixedStepSeconds=1.0;
    CHECK(!RunRoadVehicleFixture(contact,bad).completed);
    bad=fixture;
    bad.frame={7,7};
    CHECK(!RunRoadVehicleFixture(contact,bad).completed);
    bad=fixture;
    bad.steps=4000U;
    CHECK(!RunRoadVehicleFixture(contact,bad).completed);
    bad=fixture;
    bad.maximumTrianglesPerWheel=1;
    CHECK(!RunRoadVehicleFixture(contact,bad).completed);

    // A deliberate seam GAP loses some of the four wheel contacts.
    RoadJunctionScenePacket gapped=packet;
    gapped.patches.clear();
    AddSurface(gapped,{9,1},true,-1,1);
    gapped.buildSignature=13;
    auto gapPrepared=PrepareRoadContactScene(gapped);
    CHECK(gapPrepared.Ready());
    CHECK(contact.Commit(std::move(gapPrepared)));
    const auto gap=RunRoadVehicleFixture(contact,fixture);
    CHECK(gap.completed);
    CHECK(gap.stepsWithFullContact<fixture.steps);
    CHECK(gap.contactPacketSignature==13);
    contact.Clear();
    CHECK(!RunRoadVehicleFixture(contact,fixture).completed);
    return 0;
}
