#include <orbit/path_geometry/RoadJunctionPublication.hpp>
#include <orbit/path_geometry/CorridorSweep.hpp>
#include <orbit/path_geometry/TransportSections.hpp>
#include <algorithm>
#include <iostream>
#include <vector>

#define CHECK(x) do {if(!(x)){std::cerr<<"RoadJunctionPublication failed " #x " line "<<__LINE__<<'\n';return 1;}}while(false)

namespace {
using namespace orbit;
using namespace orbit::path_geometry;
PathDerivedProduct Road(scene::ObjectId id,frames::FrameId frame,
                        math::Double3 far,math::Double3 origin={})
{
    PathCenterline line{
        .edge=id,.frame=frame,.sourceRevision=7,
        .samples={{.position=origin,.up={0,1,0}},
                  {.position=origin+far,.up={0,1,0}}}
    };
    paths::PathProfile profile{
        .name="Road",.kind=paths::PathProfileKind::Road,
        .widthMeters=7.0,.lanes=2
    };
    RoadSectionDesign section;
    section.laneCount=2;
    section.laneWidthMeters=3.5;
    return BuildCorridorDerived(line,profile,MakeRoadSweep(section),
                                {.sampleSpacingMeters=2.0});
}
} // namespace

int main()
{
    using namespace orbit;
    using namespace orbit::path_geometry;
    const scene::ObjectId node{88,1};
    const frames::FrameId frame{10,20};
    auto east=Road({1,1},frame,{32,0,0});
    auto west=Road({1,2},frame,{-32,0,0});
    auto north=Road({1,3},frame,{0,0,32});
    std::vector<JunctionApproach> approaches{
        {.edge=east.edge,.frame=frame,.mode=TransportMode::Road,
         .geometry=&east},
        {.edge=west.edge,.frame=frame,.mode=TransportMode::Road,
         .geometry=&west},
        {.edge=north.edge,.frame=frame,.mode=TransportMode::Road,
         .geometry=&north}
    };
    const auto plan=BuildTransportJunction(node,approaches);
    CHECK(plan.Valid());
    const auto built=BuildRoadJunctionConstruction(plan,approaches);
    CHECK(built.Ready());
    std::vector<RoadJunctionConstruction> junctions{built};
    std::vector<const PathDerivedProduct*> sources{&east,&west,&north};
    RoadJunctionScenePublisher publisher;
    auto candidate=AssembleRoadJunctionScene(frame,12U,junctions,sources);
    CHECK(candidate.Ready());
    CHECK(candidate.packet.edges.size()==3U);
    CHECK(candidate.packet.patches.size()==1U);
    CHECK(candidate.packet.connections.size()==built.laneConnectors.size());
    CHECK(publisher.Publish(std::move(candidate)));
    CHECK(publisher.Snapshot()!=nullptr);
    const auto* initial=publisher.Snapshot();
    const auto signature=initial->buildSignature;
    CHECK(initial->worldGeneration==12U);
    for(const auto& edge:initial->edges)
    {
        const auto* original=std::find_if(sources.begin(),sources.end(),
            [&](const auto* p){return p->edge==edge.edge;})!=sources.end()?
            *std::find_if(sources.begin(),sources.end(),
                [&](const auto* p){return p->edge==edge.edge;}):nullptr;
        CHECK(original!=nullptr);
        CHECK(edge.product.visualMesh.indices.size()<
              original->visualMesh.indices.size());
        CHECK(edge.product.collision.size()==edge.product.visualMesh.indices.size()/3);
        CHECK(edge.product.stations.size()<original->stations.size());
        CHECK(edge.product.navigation.size()==edge.product.stations.size());
        CHECK(edge.product.referenceGraph.nodes.size()==
              edge.product.stations.size()*edge.product.lanes.size());
        for(const auto& lane:edge.product.lanes)
            CHECK(lane.points.size()==edge.product.stations.size());
        CHECK(edge.rawBuildSignature==original->buildSignature);
    }
    auto again=AssembleRoadJunctionScene(frame,12U,junctions,sources);
    CHECK(again.Ready());
    CHECK(again.packet.buildSignature==signature);
    std::reverse(sources.begin(),sources.end());
    auto shuffled=AssembleRoadJunctionScene(frame,12U,junctions,sources);
    CHECK(shuffled.Ready());
    CHECK(shuffled.packet.buildSignature==signature);

    // A shared trunk with junctions at BOTH ends yields one doubly cropped
    // road, not duplicate independently trimmed geometry.
    auto eastEnd=Road({1,4},frame,{32,0,0},{32,0,0});
    auto southEnd=Road({1,5},frame,{0,0,-32},{32,0,0});
    // east already runs 32 m from origin and is used at its end.
    std::vector<JunctionApproach> secondApproaches{
        {.edge=east.edge,.frame=frame,.mode=TransportMode::Road,
         .atStart=false,.geometry=&east},
        {.edge=eastEnd.edge,.frame=frame,.mode=TransportMode::Road,
         .geometry=&eastEnd},
        {.edge=southEnd.edge,.frame=frame,.mode=TransportMode::Road,
         .geometry=&southEnd}
    };
    const scene::ObjectId secondNode{88,2};
    const auto secondPlan=BuildTransportJunction(secondNode,secondApproaches);
    CHECK(secondPlan.Valid());
    const auto secondBuilt=BuildRoadJunctionConstruction(
        secondPlan,secondApproaches);
    CHECK(secondBuilt.Ready());
    std::vector<RoadJunctionConstruction> both{built,secondBuilt};
    std::vector<const PathDerivedProduct*> five{
        &east,&west,&north,&eastEnd,&southEnd};
    const auto doubleCut=AssembleRoadJunctionScene(
        frame,12U,both,five);
    CHECK(doubleCut.Ready());
    CHECK(doubleCut.packet.patches.size()==2U);
    CHECK(doubleCut.packet.edges.size()==5U);
    const auto shared=std::find_if(doubleCut.packet.edges.begin(),
        doubleCut.packet.edges.end(),[&](const auto& v) {
            return v.edge==east.edge;
        });
    CHECK(shared!=doubleCut.packet.edges.end());
    CHECK(shared->product.stations.size()<east.stations.size()-2U);
    CHECK(shared->product.collision.size()==
          shared->product.visualMesh.indices.size()/3U);

    std::vector<scene::ObjectId> dirty{east.edge};
    auto failure=AssembleRoadJunctionScene(frame,12U,junctions,sources,dirty);
    CHECK(!failure.Ready());
    CHECK(failure.findings.front().issue==RoadJunctionPublicationIssue::DirtySource);
    CHECK(!publisher.Publish(std::move(failure)));
    CHECK(publisher.Snapshot()!=nullptr);
    CHECK(publisher.Snapshot()->buildSignature==signature);

    auto changed=east;
    changed.buildSignature++;
    std::vector<const PathDerivedProduct*> stale{&changed,&west,&north};
    const auto staleResult=AssembleRoadJunctionScene(frame,12U,junctions,stale);
    CHECK(!staleResult.Ready());
    CHECK(staleResult.findings.front().issue==
          RoadJunctionPublicationIssue::ChangedSource);
    CHECK(publisher.Snapshot()->buildSignature==signature);
    CHECK(!AssembleRoadJunctionScene(frame,12U,junctions,sources,{},
                                    64U).Ready());
    std::vector<RoadJunctionConstruction> repeatedNode{built,built};
    CHECK(!AssembleRoadJunctionScene(frame,12U,repeatedNode,sources).Ready());
    CHECK(!AssembleRoadJunctionScene(frame,0U,junctions,sources).Ready());
    publisher.Clear();
    CHECK(publisher.Snapshot()==nullptr);
    return 0;
}
