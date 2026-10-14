#include <orbit/path_geometry/CorridorEarthworks.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL " #x " line " << __LINE__ << '\n'; return 1; } } while(false)
int main()
{
    using namespace orbit::path_geometry;
    const std::vector<PathStation> line{
        {.stationMeters=0,.position={0,0,0},.lateral={0,0,1}},
        {.stationMeters=100,.position={100,0,0},.lateral={0,0,1}}
    };
    const CorridorEarthworkOptions opt{.halfWidthMeters=3,.shoulderMeters=1,
        .crossSectionSamples=16,.maximumStationSpacingMeters=10};
    const auto cut=EstimateCorridorEarthworks(line,
        [](const orbit::math::Double3&) {return 2.0;},opt);
    CHECK(cut.sections.size()==11);
    CHECK(std::abs(cut.cutCubicMeters-1600.0)<1.e-8);
    CHECK(cut.fillCubicMeters==0.0);
    CHECK(std::abs(cut.evaluatedLengthMeters-100.0)<1.e-9);
    const auto fill=EstimateCorridorEarthworks(line,
        [](const orbit::math::Double3&) {return -1.5;},opt);
    CHECK(std::abs(fill.fillCubicMeters-1200.0)<1.e-8);
    CHECK(fill.cutCubicMeters==0.0);
    const auto mixed=EstimateCorridorEarthworks(line,
        [](const orbit::math::Double3& p) {return p.z>0 ? 1.0 : -1.0;},opt);
    CHECK(std::abs(mixed.cutCubicMeters-400.0)<1.e-8);
    CHECK(std::abs(mixed.fillCubicMeters-400.0)<1.e-8);
    CHECK(EstimateCorridorEarthworks({},[](const orbit::math::Double3&){return 0.0;}).sections.empty());
    bool threw=false;
    try { (void)EstimateCorridorEarthworks(line,
        [](const orbit::math::Double3&) {return std::numeric_limits<double>::quiet_NaN();});}
    catch(const std::invalid_argument&) {threw=true;}
    CHECK(threw);
    auto bad=line;bad[1].stationMeters=0;
    threw=false;
    try {(void)EstimateCorridorEarthworks(bad,
        [](const orbit::math::Double3&){return 0.0;});}
    catch(const std::invalid_argument&) {threw=true;}
    CHECK(threw);
    return 0;
}
