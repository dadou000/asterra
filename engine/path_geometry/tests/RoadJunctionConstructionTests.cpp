#include <orbit/path_geometry/RoadJunctionConstruction.hpp>
#include <orbit/path_geometry/CorridorSweep.hpp>
#include <orbit/path_geometry/TransportSections.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

#define CHECK(e) do { if(!(e)) { std::cerr<<"RoadJunctionConstruction: " #e " at "<<__LINE__<<'\n';return 1;} } while(false)

namespace
{
using namespace orbit;
using namespace orbit::path_geometry;
PathDerivedProduct Road(scene::ObjectId edge,frames::FrameId frame,
                        math::Double3 end,f64 stationSpacing=2.0)
{
    PathCenterline line{
        .edge=edge,.frame=frame,.sourceRevision=7,
        .samples={{.position={0,0,0},.up={0,1,0}},
                  {.position=end,.up={0,1,0}}}
    };
    paths::PathProfile profile{
        .name="Road",.kind=paths::PathProfileKind::Road,
        .widthMeters=7.0,.lanes=2
    };
    RoadSectionDesign design;
    design.laneCount=2;
    design.laneWidthMeters=3.5;
    return BuildCorridorDerived(line,profile,MakeRoadSweep(design),
                                {.sampleSpacingMeters=stationSpacing});
}
bool CollisionMatches(const PathMesh& mesh,
                     const std::vector<CollisionTriangle>& triangles)
{
    if(mesh.indices.size()!=triangles.size()*3U)return false;
    for(std::size_t i=0;i<triangles.size();++i)
    {
        const auto a=mesh.indices[i*3U],b=mesh.indices[i*3U+1U],
                   c=mesh.indices[i*3U+2U];
        if(a>=mesh.vertices.size()||b>=mesh.vertices.size()||
           c>=mesh.vertices.size())return false;
        if(triangles[i].a!=mesh.vertices[a].position ||
           triangles[i].b!=mesh.vertices[b].position ||
           triangles[i].c!=mesh.vertices[c].position)return false;
    }
    return true;
}
}
int main()
{
    using namespace orbit;
    using namespace orbit::path_geometry;
    const scene::ObjectId node{51,22};
    const frames::FrameId frame{2,3};
    auto east=Road({10,1},frame,{32,0,0});
    auto west=Road({10,2},frame,{-32,0,0});
    auto north=Road({10,3},frame,{0,0,32});
    std::vector<JunctionApproach> approaches{
        {east.edge,frame,TransportMode::Road,true,true,&east},
        {west.edge,frame,TransportMode::Road,true,true,&west},
        {north.edge,frame,TransportMode::Road,true,true,&north}
    };
    const auto plan=BuildTransportJunction(node,approaches);
    CHECK(plan.Valid());
    CHECK(plan.movements.size()==6U);
    const auto built=BuildRoadJunctionConstruction(plan,approaches);
    CHECK(built.Ready());
    CHECK(built.frame==frame);
    CHECK(built.edges.size()==3U);
    CHECK(built.laneConnectors.size()==plan.movements.size());
    CHECK(built.pavement.indices.size()>=9U);
    CHECK(CollisionMatches(built.pavement,built.pavementCollision));
    for(const auto& strip:built.edges)
    {
        CHECK(strip.retainedMesh.vertices.size()>0U);
        CHECK(strip.retainedMesh.indices.size()>0U);
        CHECK(CollisionMatches(strip.retainedMesh,strip.collision));
        const PathDerivedProduct* source=nullptr;
        if(strip.edge==east.edge)source=&east;
        if(strip.edge==west.edge)source=&west;
        if(strip.edge==north.edge)source=&north;
        CHECK(source!=nullptr);
        CHECK(strip.retainedMesh.indices.size()<source->visualMesh.indices.size());
        CHECK(strip.sourceCutStation>0U);
        CHECK(strip.sourceCutStation+1U<source->stations.size());
        // Exact stitch of the complete 3+2+2 profile, including
        // both outer shoulders and the central crown.
        const auto n=source->stations.size();
        const std::size_t k=strip.sourceCutStation;
        const std::size_t boundary[]{
            3*n+2*k,3*k,3*k+1,3*k+2,5*n+2*k+1
        };
        for(const auto vertex:boundary)
        {
            const auto point=source->visualMesh.vertices[vertex].position;
            CHECK(std::any_of(built.pavement.vertices.begin(),
                built.pavement.vertices.end(),
                [&](const auto& v){
                    return v.position==point &&
                        v.normal==source->visualMesh.vertices[vertex].normal;
                }));
        }
    }
    for(const auto& move:built.laneConnectors)
    {
        CHECK(move.points.size()==13U);
        CHECK(move.lengthMeters>0.0 && std::isfinite(move.lengthMeters));
        CHECK(math::LengthSquared(move.points.front()-move.points.back())>1.e-8);
        const auto input=std::find_if(built.edges.begin(),built.edges.end(),
            [&](const auto& strip){return strip.edge==move.incomingEdge;});
        const auto output=std::find_if(built.edges.begin(),built.edges.end(),
            [&](const auto& strip){return strip.edge==move.outgoingEdge;});
        CHECK(input!=built.edges.end() && output!=built.edges.end());
        const auto* ia=move.incomingEdge==east.edge?&east:
                       move.incomingEdge==west.edge?&west:&north;
        const auto* ib=move.outgoingEdge==east.edge?&east:
                       move.outgoingEdge==west.edge?&west:&north;
        CHECK(move.points.front()==ia->lanes[move.incomingLane].
              points[input->sourceCutStation].position);
        CHECK(move.points.back()==ib->lanes[move.outgoingLane].
              points[output->sourceCutStation].position);
    }
    const auto again=BuildRoadJunctionConstruction(plan,approaches);
    CHECK(again.Ready());
    CHECK(built.dependencySignature!=0U);
    CHECK(again.dependencySignature==built.dependencySignature);
    CHECK(again.pavement==built.pavement);
    CHECK(again.pavementCollision==built.pavementCollision);
    CHECK(again.laneConnectors.size()==built.laneConnectors.size());
    std::reverse(approaches.begin(),approaches.end());
    const auto shuffled=BuildRoadJunctionConstruction(plan,approaches);
    CHECK(shuffled.Ready());
    CHECK(shuffled.dependencySignature==built.dependencySignature);
    CHECK(shuffled.pavement==built.pavement);
    CHECK(shuffled.edges.size()==built.edges.size());

    // Geometry changes invalidate the entire revision-fenced replacement.
    auto editedEast=Road({10,1},frame,{36,0,0});
    auto modified=approaches;
    for(auto& a:modified)
        if(a.edge==east.edge)a.geometry=&editedEast;
    const auto modifiedPlan=BuildTransportJunction(node,modified);
    CHECK(modifiedPlan.Valid());
    const auto modifiedProduct=
        BuildRoadJunctionConstruction(modifiedPlan,modified);
    CHECK(modifiedProduct.Ready());
    CHECK(modifiedProduct.dependencySignature!=built.dependencySignature);

    // An invalid connector detected AFTER patch staging must return no
    // publishable geometry. No partial mesh/contact generation may leak.
    auto brokenPlan=plan;
    brokenPlan.movements.push_back({
        .incomingEdge={99,99},
        .outgoingEdge=east.edge,
        .mode=TransportMode::Road
    });
    const auto brokenBuild=BuildRoadJunctionConstruction(
        brokenPlan,approaches);
    CHECK(!brokenBuild.Ready());
    CHECK(brokenBuild.pavement.vertices.empty());
    CHECK(brokenBuild.pavementCollision.empty());
    CHECK(brokenBuild.edges.empty());
    CHECK(brokenBuild.dependencySignature==0U);

    auto tooSmall=BuildRoadJunctionConstruction(plan,approaches,
        {.trimDistanceMeters=500.0});
    CHECK(!tooSmall.Ready());
    const auto tooLarge=BuildRoadJunctionConstruction(plan,approaches,
        {.maximumInputVertices=32U});
    CHECK(!tooLarge.Ready());
    const auto tooSmallOutput=BuildRoadJunctionConstruction(plan,approaches,
        {.maximumOutputVertices=32U});
    CHECK(!tooSmallOutput.Ready());
    // Reject non-star-shaped/incomplete two-arm acute approaches instead of
    // leaving overlapping patch and corridor collider triangles.
    auto northeast=Road({10,4},frame,{28,0,6});
    std::vector<JunctionApproach> acute{
        {east.edge,frame,TransportMode::Road,true,true,&east},
        {northeast.edge,frame,TransportMode::Road,true,true,&northeast}
    };
    const auto acutePlan=BuildTransportJunction(node,acute);
    CHECK(acutePlan.Valid());
    CHECK(!BuildRoadJunctionConstruction(acutePlan,acute).Ready());

    auto rail=approaches;
    rail[0].mode=TransportMode::Rail;
    CHECK(!BuildRoadJunctionConstruction(plan,rail).Ready());
    auto foreign=approaches;
    foreign[0].frame={5,5};
    CHECK(!BuildRoadJunctionConstruction(plan,foreign).Ready());
    const std::vector<JunctionApproach> onlyOne{approaches.front()};
    CHECK(!BuildRoadJunctionConstruction(plan,onlyOne).Ready());
    return 0;
}
