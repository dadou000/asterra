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
    return 0;
}
