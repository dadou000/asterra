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
    // Road construction influence is bounded and reproducible; it does not
    // modify the terrain source or depend on the sampling order.
    const CorridorTerrainFootprint footprint{3.5, 1.0, 3.0};
    const auto onCenter = SampleCorridorTerrain(stations, {5, 15, 0}, footprint);
    CHECK(onCenter.has_value());
    CHECK(onCenter->influence == 1.0);
    CHECK(std::abs(onCenter->targetHeightMeters) < 1.e-9);
    const auto shoulder = SampleCorridorTerrain(stations, {5, 0, 5.5}, footprint);
    CHECK(shoulder.has_value());
    CHECK(shoulder->influence > 0.0 && shoulder->influence < 1.0);
    CHECK(!SampleCorridorTerrain(stations, {5, 0, 10}, footprint).has_value());
    CHECK(!SampleCorridorTerrain(stations, {5, 0, 0}, {3.5, -1.0, 2.0}).has_value());
    CHECK(onCenter == SampleCorridorTerrain(stations, {5, 15, 0}, footprint));
    const auto cut = BlendCorridorTerrainHeight(12.0, *onCenter);
    CHECK(std::abs(cut.heightMeters) < 1.e-9);
    CHECK(std::abs(cut.cutMeters-12.0) < 1.e-9);
    CHECK(cut.fillMeters == 0.0);
    const auto fill = BlendCorridorTerrainHeight(-8.0, *onCenter);
    CHECK(fill.cutMeters == 0.0 && std::abs(fill.fillMeters-8.0)<1.e-9);
    const auto partial = BlendCorridorTerrainHeight(10.0, *shoulder);
    CHECK(partial.heightMeters > 0.0 && partial.heightMeters < 10.0);
    const auto envelope = CorridorFootprintEnvelope(stations, footprint);
    CHECK(envelope.has_value());
    CHECK(envelope->minimum.x <= -7.5);
    CHECK(envelope->maximum.x >= 27.5);
    CHECK(!CorridorFootprintEnvelope(stations, {3.5, 0.0, -1.0}).has_value());

    const auto roadMesh=BuildCorridorMesh(stations,road);
    CHECK(roadMesh.vertices.size()==9);
    CHECK(roadMesh.indices.size()==24);
    CHECK(roadMesh.vertices[0].normal.y > 0.9);
    CHECK(roadMesh.vertices[1].normal.y > 0.9);
    CHECK(std::abs(roadMesh.vertices[0].normal.z) > 0.005);
    CHECK(std::abs(roadMesh.vertices[2].normal.z) > 0.005);
    CHECK(roadMesh.vertices[0].normal.z * roadMesh.vertices[2].normal.z < 0.0);

    CHECK(roadMesh.vertices[1].position.y==0.07);
    // Rails use the exact same multi-band sweep, never a second mesher.
    CorridorSweep rails{{{{{-0.77,0},{-0.73,0.15}}},{{{0.73,0.15},{0.77,0}}}}, {0,0.05,0.1}};
    const auto railMesh=BuildCorridorMesh(stations,rails);
    CHECK(railMesh.vertices.size()==12);
    CHECK(railMesh.indices.size()==24);
    CHECK(railMesh.vertices[0].normal.y > 0.0);
    CHECK(railMesh.vertices[0].normal != roadMesh.vertices[0].normal);

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
    PathCenterline source{};
    source.edge=orbit::scene::ObjectId::Random();
    source.frame=orbit::frames::FrameId::Random();
    source.sourceRevision=3;
    source.samples={
        {.position={0,0,0}, .up={0,1,0}},
        {.position={20,0,0}, .up={0,1,0}}
    };
    orbit::paths::PathProfile profile{};
    profile.widthMeters=6;
    profile.lanes=2;
    const auto basic=BuildPathDerived(source,profile);
    CorridorSweep section{{{{{-3,0},{0,0.1},{3,0}}}}, {}};
    const auto derived=BuildCorridorDerived(source,profile,section);
    CHECK(derived.visualMesh.vertices.size()>basic.visualMesh.vertices.size());
    CHECK(derived.collision.size()==derived.visualMesh.indices.size()/3);
    CHECK(derived.navigation.size()==derived.stations.size());
    CHECK(derived.lanes.size()==profile.lanes);
    CHECK(derived.lanes.front().points.size()==derived.stations.size());
    CHECK(derived.referenceGraph.nodes.size()==derived.stations.size()*profile.lanes);
    // Crowned pavement must raise the lane reference with the real surface.
    CHECK(std::abs(derived.lanes[0].points[1].position.y-0.05)<1.e-8);
    CHECK(derived.buildSignature!=basic.buildSignature);
    CHECK(derived==BuildCorridorDerived(source,profile,section));
    // Authored width keys must carry lane centres and navigation width.
    CorridorSweep tapered{{{{{-3,0},{0,0.1},{3,0}}}},{}};
    tapered.sectionKeys={
        {0, {{{{-3,0},{0,0.1},{3,0}}}}},
        {20, {{{{-5,0},{0,0.1},{5,0}}}}}
    };
    const auto widening=BuildCorridorDerived(source,profile,tapered);
    CHECK(std::abs(widening.lanes[0].points.front().position.z+1.5)<1.e-9);
    CHECK(std::abs(widening.lanes[0].points.back().position.z+2.5)<1.e-9);
    CHECK(std::abs(widening.lanes[1].points.back().position.z-2.5)<1.e-9);
    CHECK(std::abs(widening.navigation.front().halfWidthMeters-3.0)<1.e-9);
    CHECK(std::abs(widening.navigation.back().halfWidthMeters-5.0)<1.e-9);
    section.bands[0].profile[1].elevationMeters=0.2;
    CHECK(BuildCorridorDerived(source,profile,section).buildSignature!=derived.buildSignature);
    // Equal profile scalars but different band boundaries must not alias.
    CorridorSweep oneBand{{{{{-2,0},{-1,0},{1,0},{2,0}}}}, {}};
    CorridorSweep twoBands{{{{{-2,0},{-1,0}}},{{{1,0},{2,0}}}}, {}};
    CHECK(BuildCorridorDerived(source, profile, oneBand).buildSignature !=
          BuildCorridorDerived(source, profile, twoBands).buildSignature);
    // A body-centred corridor maps to stable planetary physical page IDs.
    orbit::world::PlanetDefinition planet{};
    planet.id = orbit::world::PlanetId::Random();
    planet.radiusMeters = 6000000.0;
    const std::vector<orbit::math::Double3> bodyPath{
        {6000000, 0, 0}, {6000000, 0, 500}
    };
    const auto pages = ProjectCorridorToTerrainPages(
        planet, bodyPath, 5.0, 10, 4096);
    CHECK(pages.has_value() && !pages->empty());
    CHECK(pages == ProjectCorridorToTerrainPages(
        planet, bodyPath, 5.0, 10, 4096));
    CHECK(!ProjectCorridorToTerrainPages(
        planet, bodyPath, 5.0, 10, 1).has_value());
    CHECK(!ProjectCorridorToTerrainPages(
        planet, bodyPath, -1.0, 10).has_value());
    return 0;
}
