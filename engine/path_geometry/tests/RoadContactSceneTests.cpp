#include <orbit/path_geometry/RoadContactScene.hpp>

#include <cmath>
#include <iostream>
#include <stdexcept>
#define CHECK(e) do { if(!(e)){std::cerr<<"RoadContactScene: " #e " at "<<__LINE__<<'\n';return 1;} }while(false)

int main()
{
    using namespace orbit;
    using namespace orbit::path_geometry;
    const frames::FrameId frame{5,4};
    RoadJunctionScenePacket packet;
    packet.frame=frame;
    packet.worldGeneration=7U;
    packet.buildSignature=91U;
    RoadJunctionPublishedEdge road;
    road.edge={10,1};
    road.product.edge=road.edge;
    road.product.frame=frame;
    road.product.visualMesh.vertices={
        {.position={-10,0,-4}},
        {.position={-2,0,-4}},
        {.position={-2,0,4}},
        {.position={-10,0,4}}
    };
    road.product.visualMesh.indices={0U,1U,2U,0U,2U,3U};
    road.product.collision={
        {{-10,0,-4},{-2,0,-4},{-2,0,4}},
        {{-10,0,-4},{-2,0,4},{-10,0,4}}
    };
    packet.edges.push_back(road);
    RoadJunctionPatchProduct patch;
    patch.node={20,1};
    patch.frame=frame;
    patch.mesh.vertices={
        {.position={-2,0,-4}},
        {.position={3,0,-4}},
        {.position={3,0,4}},
        {.position={-2,0,4}}
    };
    patch.mesh.indices={0U,1U,2U,0U,2U,3U};
    patch.collision={
        {{-2,0,-4},{3,0,-4},{3,0,4}},
        {{-2,0,-4},{3,0,4},{-2,0,4}}
    };
    packet.patches.push_back(patch);
    RoadContactSceneOwner owner;
    auto prepared=PrepareRoadContactScene(packet);
    CHECK(prepared.Ready());
    CHECK(prepared.generation.shapes.size()==2U);
    CHECK(prepared.generation.triangleCount==4U);
    CHECK(owner.Commit(std::move(prepared)));
    CHECK(owner.Snapshot()!=nullptr);
    CHECK(owner.Snapshot()->worldGeneration==7U);
    CHECK(owner.Snapshot()->packetSignature==91U);
    auto hit=owner.Probe({.frame=frame,.origin={0,12,0},
                          .direction={0,-3,0}});
    CHECK(hit.hit && hit.junctionPatch && hit.owner==patch.node);
    CHECK(std::abs(hit.distanceMeters-12.0)<1.e-8);
    CHECK(std::abs(hit.normal.y-1.0)<1.e-8);
    // Off-patch road reaches the independently owned trimmed strip.
    hit=owner.Probe({.frame=frame,.origin={-5,5,0}});
    CHECK(hit.hit && !hit.junctionPatch && hit.owner==road.edge);
    // Broadphase ignores the distant road; one shape's triangles remain.
    CHECK(hit.testedTriangles==2U);
    auto missing=owner.Probe({.frame=frame,.origin={100,5,100}});
    CHECK(!missing.hit && !missing.budgetExceeded);
    CHECK(missing.testedTriangles==0U);

    // Stage a malformed new generation. It must NOT evict the current
    // complete one, nor allow invisible collision shapes to appear.
    auto invalid=packet;
    invalid.buildSignature=92U;
    invalid.patches[0].collision[0].a.x+=0.5;
    auto failed=PrepareRoadContactScene(invalid);
    CHECK(!failed.Ready());
    CHECK(!owner.Commit(std::move(failed)));
    CHECK(owner.Snapshot()->packetSignature==91U);
    CHECK(owner.Probe({.frame=frame,.origin={0,8,0}}).hit);

    invalid=packet;
    invalid.patches[0].node=road.edge;
    CHECK(!PrepareRoadContactScene(invalid).Ready());
    invalid=packet;
    invalid.patches[0].frame={9,9};
    CHECK(!PrepareRoadContactScene(invalid).Ready());
    CHECK(!PrepareRoadContactScene(packet,1U).Ready());
    CHECK(!PrepareRoadContactScene(packet,512U,3U).Ready());
    CHECK(!PrepareRoadContactScene(packet,512U,0U).Ready());

    packet.buildSignature=93U;
    auto next=PrepareRoadContactScene(packet);
    CHECK(next.Ready());
    CHECK(owner.Commit(std::move(next)));
    CHECK(owner.Snapshot()->packetSignature==93U);
    bool badFrame=false;
    try {static_cast<void>(owner.Probe({
        .frame={3,3},.origin={0,10,0}}));}
    catch(const std::invalid_argument&){badFrame=true;}
    CHECK(badFrame);
    auto capped=owner.Probe({
        .frame=frame,.origin={0,10,0},.maximumTriangles=1U});
    CHECK(capped.budgetExceeded);
    owner.Clear();
    CHECK(owner.Snapshot()==nullptr);
    return 0;
}
