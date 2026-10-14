#include <orbit/path_geometry/TransportJunctions.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>
#define CHECK(x) do {if (!(x)) {std::cerr<<"FAIL " #x " line "<<__LINE__<<'\n';return 1;}} while(false)

namespace
{
orbit::path_geometry::PathDerivedProduct MakeEdge(
    orbit::scene::ObjectId id,orbit::frames::FrameId frame,
    orbit::math::Double3 far, bool startAtNode=true)
{
    using namespace orbit;
    using namespace orbit::path_geometry;
    PathDerivedProduct p;
    p.edge=id;
    p.frame=frame;
    p.widthMeters=6.0;
    p.laneCount=1;
    const auto start=startAtNode?math::Double3{}:far;
    const auto end=startAtNode?far:math::Double3{};
    const auto tangent=math::Normalize(end-start);
    const auto up=math::Double3{0,1,0};
    const auto lateral=math::Normalize(math::Cross(tangent,up));
    p.stations={
        {.stationMeters=0,.position=start,.tangent=tangent,.up=up,.lateral=lateral},
        {.stationMeters=math::Length(far),.position=end,
         .tangent=tangent,.up=up,.lateral=lateral}
    };
    p.navigation={
        {.stationMeters=0,.position=start,.tangent=tangent,.halfWidthMeters=3,.lanes=1},
        {.stationMeters=math::Length(far),.position=end,
         .tangent=tangent,.halfWidthMeters=3,.lanes=1}
    };
    LaneReference lane;
    lane.laneIndex=0;
    lane.points={{0,start,tangent},{math::Length(far),end,tangent}};
    p.lanes.push_back(std::move(lane));
    return p;
}
}
int main()
{
    using namespace orbit;
    using namespace orbit::path_geometry;
    const scene::ObjectId node{8,8};
    const frames::FrameId frame{4,4},anotherFrame{5,5};
    auto east=MakeEdge({1,1},frame,{20,0,0});
    auto west=MakeEdge({1,2},frame,{-20,0,0});
    auto north=MakeEdge({1,3},frame,{0,0,20});
    const std::vector<JunctionApproach> approaches{
        {{1,1},frame,TransportMode::Road,true,true,&east},
        {{1,2},frame,TransportMode::Road,true,true,&west},
        {{1,3},frame,TransportMode::Road,true,true,&north}
    };
    const auto three=BuildTransportJunction(node,approaches);
    CHECK(three.Valid());
    CHECK(three.movements.size()==6);
    CHECK(three.validEdges.size()==3);
    CHECK(three.roadFootprint.vertices.size()>=4);
    CHECK(three.roadFootprint.indices.size()>=9);
    for(const auto& move:three.movements)
    {
        CHECK(move.incomingEdge!=move.outgoingEdge);
        CHECK(move.points.size()==13);
        CHECK(move.lengthMeters>0);
        CHECK(std::isfinite(move.lengthMeters));
    }
    const auto twice=BuildTransportJunction(node,approaches);
    CHECK(twice.movements.size()==three.movements.size());
    CHECK(twice.roadFootprint==three.roadFootprint);
    auto reversed=approaches;
    std::reverse(reversed.begin(),reversed.end());
    const auto same=BuildTransportJunction(node,reversed);
    CHECK(same.movements.size()==three.movements.size());
    for(std::size_t i=0;i<same.movements.size();++i)
        CHECK(same.movements[i].incomingEdge==three.movements[i].incomingEdge &&
              same.movements[i].outgoingEdge==three.movements[i].outgoingEdge);
    reversed=approaches;
    reversed[0].bidirectional=false;
    const auto oneWay=BuildTransportJunction(node,reversed);
    CHECK(oneWay.Valid());
    CHECK(oneWay.movements.size()==4);
    for(const auto& move:oneWay.movements)
        CHECK(move.incomingEdge!=east.edge);
    reversed=approaches;
    reversed[2].mode=TransportMode::Rail;
    const auto crossing=BuildTransportJunction(node,reversed);
    CHECK(crossing.Valid());
    CHECK(crossing.movements.size()==2); // no road-to-rail transfers
    reversed=approaches;
    auto foreignNorth=north;
    foreignNorth.frame=anotherFrame;
    reversed[2].frame=anotherFrame;
    reversed[2].geometry=&foreignNorth;
    const auto frameError=BuildTransportJunction(node,reversed);
    CHECK(!frameError.Valid());
    CHECK(frameError.findings.back().issue==JunctionIssue::MixedFrames);
    reversed=approaches;
    reversed[2].geometry=nullptr;
    const auto geometryError=BuildTransportJunction(node,reversed);
    CHECK(!geometryError.Valid());
    reversed=approaches;
    const auto limited=BuildTransportJunction(node,reversed,
        {.maximumApproaches=2});
    CHECK(!limited.Valid());
    CHECK(limited.findings.front().issue==JunctionIssue::TooManyApproaches);
    const auto limitedMoves=BuildTransportJunction(node,approaches,
        {.maximumMovements=2});
    CHECK(!limitedMoves.Valid());
    CHECK(limitedMoves.movements.size()==2);
    return 0;
}
