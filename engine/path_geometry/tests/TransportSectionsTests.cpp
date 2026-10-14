#include <orbit/path_geometry/TransportSections.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

#define CHECK(x) do { if (!(x)) {std::cerr << "FAIL " #x " at " << __LINE__ << '\n'; return 1;} } while(false)

int main()
{
    using namespace orbit::path_geometry;
    const std::vector<PathStation> stations{
        {.stationMeters=0.0,.position={0,0,0}},
        {.stationMeters=12.0,.position={12,0,0}},
        {.stationMeters=24.0,.position={24,0,0}}
    };
    const auto road=MakeRoadSweep({});
    CHECK(road.bands.size()==3);
    CHECK(road.bands.front().profile.size()==3);
    const auto pavement=BuildCorridorMesh(stations,road);
    CHECK(pavement.indices.size()==48U);
    CHECK(!pavement.vertices.empty());
    const RailSectionDesign railway{.trackCount=2};
    const std::vector<double> cant{0.0,0.04,0.0};
    const auto sweep=MakeRailSweep(railway,cant);
    CHECK(sweep.bands.size()==5);
    const auto contactMesh=BuildCorridorMesh(stations,sweep);
    CHECK(contactMesh.indices.size()==72U);
    const auto contacts=RailContactReferences(stations,railway,cant);
    CHECK(contacts.size()==6U);
    for (const auto& c:contacts)
        CHECK(std::abs(math::Length(c.rightRail-c.leftRail) -
                       railway.gaugeMeters)<1.e-10);
    CHECK(std::abs(contacts[2].rightRail.y-contacts[2].leftRail.y)>0.01);
    const auto sleepers=RailSleeperPlacements(stations,railway,cant);
    CHECK(sleepers.size()==82U);
    CHECK(std::abs(sleepers[2].stationMeters-0.6)<1.e-10);
    CHECK(std::abs(sleepers[2].centre.x-0.6)<1.e-10);
    const auto repeat=RailSleeperPlacements(stations,railway,cant);
    CHECK(repeat[60].centre==sleepers[60].centre);
    bool rejected=false;
    try { static_cast<void>(RailSleeperPlacements(stations,railway,{},10)); }
    catch (const std::length_error&) { rejected=true; }
    CHECK(rejected);
    rejected=false;
    try {static_cast<void>(RailContactReferences(stations,railway,{0.0}));}
    catch(const std::invalid_argument&) {rejected=true;}
    CHECK(rejected);
    rejected=false;
    try {
        RailSectionDesign invalid;
        invalid.gaugeMeters=std::numeric_limits<double>::quiet_NaN();
        static_cast<void>(MakeRailSweep(invalid));
    } catch(const std::invalid_argument&) {rejected=true;}
    CHECK(rejected);
    return 0;
}
