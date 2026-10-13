#include <orbit/path_geometry/CorridorSweep.hpp>
#include <cmath>
#include <iostream>

#define CHECK(x) do { if (!(x)) { std::cerr << "Failed: " #x << " line " << __LINE__ << '\n'; return 1; } } while(false)

int main()
{
    using namespace orbit::path_geometry;
    std::vector<PathStation> stations{
        {.stationMeters=0, .position={0,0,0}, .tangent={1,0,0}, .up={0,1,0}, .lateral={0,0,1}},
        {.stationMeters=10, .position={10,0,0}, .tangent={1,0,0}, .up={0,1,0}, .lateral={0,0,1}},
        {.stationMeters=20, .position={20,0,5}, .tangent={1,0,0}, .up={0,1,0}, .lateral={0,0,1}}
    };
    CorridorSweep road{{{{{-3.5,0},{0,0.07},{3.5,0}}}}, {0,0,0}};
    const auto roadMesh=BuildCorridorMesh(stations,road);
    CHECK(roadMesh.vertices.size()==9);
    CHECK(roadMesh.indices.size()==24);
    CHECK(roadMesh.vertices[1].position.y==0.07);
    // Rails use the exact same multi-band sweep, never a second mesher.
    CorridorSweep rails{{{{{-0.77,0},{-0.73,0.15}}},{{{0.73,0.15},{0.77,0}}}}, {0,0.05,0.1}};
    const auto railMesh=BuildCorridorMesh(stations,rails);
    CHECK(railMesh.vertices.size()==12);
    CHECK(railMesh.indices.size()==24);
    CHECK(railMesh.vertices[4].position.y != railMesh.vertices[0].position.y);
    CHECK(BuildCorridorMesh(stations,rails)==railMesh);
    bool rejected=false;
    try { CorridorSweep bad{{{{{1,0},{0,0}}}}, {}}; (void)BuildCorridorMesh(stations,bad); }
    catch(const std::invalid_argument&) { rejected=true; }
    CHECK(rejected);
    // Curvature, length and cant generate extra stations only where needed.
    auto refined = RefineCorridor(stations, rails,
        {.maximumChordErrorMeters=0.01, .maximumBankStepRadians=0.015,
         .maximumSegmentLengthMeters=4.0, .maximumSubdivisions=10});
    CHECK(refined.stations.size()>stations.size());
    CHECK(refined.stations.front().position==stations.front().position);
    CHECK(refined.stations.back().position==stations.back().position);
    CHECK(refined.corridor.bankRadians.size()==refined.stations.size());
    CHECK(BuildCorridorMesh(refined.stations,refined.corridor).vertices.size() >
          railMesh.vertices.size());
    auto again=RefineCorridor(stations,rails,
        {.maximumChordErrorMeters=0.01, .maximumBankStepRadians=0.015,
         .maximumSegmentLengthMeters=4.0, .maximumSubdivisions=10});
    CHECK(refined.stations==again.stations);
    // Variable road width and crown retain the same vertex topology.
    CorridorSweep varying=road;
    varying.sectionKeys={
        {0, {{{{-3.0,0},{0,0.06},{3.0,0}}}}},
        {20, {{{{-5.0,0},{0,0.12},{5.0,0}}}}}
    };
    const auto varMesh=BuildCorridorMesh(stations,varying);
    CHECK(varMesh.vertices.size()==roadMesh.vertices.size());
    CHECK(std::abs(varMesh.vertices[0].position.z + 3.0)<1.e-9);
    CHECK(std::abs(varMesh.vertices[3].position.z + 4.0)<1.e-9);
    CHECK(std::abs(varMesh.vertices[7].position.y - 0.12)<1.e-9);
    CHECK(BuildCorridorMesh(stations,varying)==varMesh);
    // Refining the path must preserve the authored section keys.
    const auto varRefined=RefineCorridor(stations,varying);
    CHECK(varRefined.corridor.sectionKeys.size()==2);
    CHECK(BuildCorridorMesh(varRefined.stations,varRefined.corridor).vertices.size() >
          varMesh.vertices.size());
    bool badKey=false;
    try {
        auto broken=varying;
        broken.sectionKeys[1].bands[0].profile.pop_back();
        (void)BuildCorridorMesh(stations,broken);
    } catch (const std::invalid_argument&) {badKey=true;}
    CHECK(badKey);
    return 0;
}
