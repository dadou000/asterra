#include <orbit/path_geometry/InfrastructureSectionBuilder.hpp>
#include <cmath>
#include <iostream>
#include <stdexcept>

#define CHECK(x) do {if (!(x)) {std::cerr<<"FAIL " #x " at "<<__LINE__<<'\n';return 1;}} while(false)

int main()
{
    using namespace orbit;
    using namespace orbit::path_geometry;
    PathCenterline line{};
    line.edge = scene::ObjectId::Random();
    line.frame = frames::FrameId::Random();
    line.sourceRevision = 5;
    line.samples = {
        {.position={0,0,0}},
        {.position={100,0,0}}
    };
    paths::PathProfile road{};
    road.widthMeters=7.0;
    road.lanes=2;
    road.kind=paths::PathProfileKind::Road;
    const auto roadPlan=PlanInfrastructureSection(line,road);
    CHECK(roadPlan.section.bands.size()==3U);
    CHECK(roadPlan.section.bands.front().profile.front().lateralMeters==-3.5);
    const auto pavement=BuildCorridorDerived(line,road,roadPlan.section);
    CHECK(pavement.lanes.size()==2U);
    CHECK(pavement.lanes[0].points.size()==pavement.stations.size());
    CHECK(pavement.collision.size()==pavement.visualMesh.indices.size()/3U);

    paths::PathProfile rail{};
    rail.kind=paths::PathProfileKind::Rail;
    rail.widthMeters=4.035;
    rail.lanes=1;
    const auto noCant=PlanInfrastructureSection(line,rail,{},22.0,false);
    CHECK(noCant.section.bankRadians.empty());
    CHECK(noCant.section.bands.size()==3U);
    const auto withCant=PlanInfrastructureSection(line,rail,{},22.0,true);
    CHECK(withCant.section.bankRadians.size()==withCant.engineering.stations.size());
    CHECK(withCant.railwayCant.Valid());
    CHECK(withCant.railwayCant.stations.size()==withCant.engineering.stations.size());
    const auto track=BuildCorridorDerived(line,rail,withCant.section);
    CHECK(track.visualMesh.vertices.size()>0U);
    CHECK(track.collision.size()==track.visualMesh.indices.size()/3U);
    bool rejected=false;
    try {rail.widthMeters=1.0;static_cast<void>(PlanInfrastructureSection(line,rail));}
    catch(const std::invalid_argument&) {rejected=true;}
    CHECK(rejected);
    rejected=false;
    try {static_cast<void>(PlanInfrastructureSection(line,road,{},-1.0));}
    catch(const std::invalid_argument&) {rejected=true;}
    CHECK(rejected);
    return 0;
}
