#include <orbit/path_geometry/RailCantDesign.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#define CHECK(x) do { if (!(x)) {std::cerr << "FAIL " #x " at " << __LINE__ << '\n'; return 1;} } while(false)

int main()
{
    using namespace orbit::path_geometry;
    CorridorEngineeringReport straight;
    straight.stations={
        {.stationMeters=0,.curvaturePerMeter=0},
        {.stationMeters=20,.curvaturePerMeter=0},
        {.stationMeters=40,.curvaturePerMeter=0}
    };
    const auto flat=DesignRailCant(straight);
    CHECK(flat.Valid());
    CHECK(flat.findings.empty());
    CHECK(flat.stations.size()==3U);
    CHECK(flat.stations[1].suggestedCantMeters==0.0);
    CHECK(flat.stations[1].remainingDeficiencyMeters==0.0);
    const RailCantLimits limits{};
    CorridorEngineeringReport reverse;
    reverse.stations={
        {.stationMeters=0,.curvaturePerMeter=0},
        {.stationMeters=20,.curvaturePerMeter=0.001},
        {.stationMeters=40,.curvaturePerMeter=0.001},
        {.stationMeters=60,.curvaturePerMeter=-0.001},
        {.stationMeters=80,.curvaturePerMeter=-0.001},
        {.stationMeters=100,.curvaturePerMeter=0}
    };
    const auto plan=DesignRailCant(reverse,limits);
    CHECK(plan.Valid());
    CHECK(plan.stations.size()==reverse.stations.size());
    CHECK(plan.stations[1].suggestedCantMeters>0.0);
    CHECK(plan.stations[4].suggestedCantMeters<0.0);
    for (std::size_t i=1;i<plan.stations.size();++i)
    {
        const auto delta=plan.stations[i].stationMeters-
            plan.stations[i-1].stationMeters;
        CHECK(std::abs(plan.stations[i].suggestedCantMeters-
            plan.stations[i-1].suggestedCantMeters)
            <=limits.maximumCantGradient*delta+1.e-12);
        CHECK(std::abs(std::sin(plan.stations[i].suggestedBankRadians)*
            limits.gaugeMeters-plan.stations[i].suggestedCantMeters)<1.e-12);
    }
    const auto repeat=DesignRailCant(reverse,limits);
    CHECK(repeat.stations.size()==plan.stations.size());
    CHECK(repeat.stations[2].suggestedCantMeters==
          plan.stations[2].suggestedCantMeters);
    // A demanding high-speed tight curve must flag impossible shared
    // fast-passenger / slow-freight cant limits, not quietly approve.
    auto impossible=straight;
    impossible.stations[1].curvaturePerMeter=0.025;
    const auto warning=DesignRailCant(impossible,limits);
    bool incompatible=false;
    for (const auto& finding:warning.findings)
        incompatible|=finding.issue==RailCantIssue::IncompatibleSpeeds;
    CHECK(incompatible);
    RailCantLimits bad=limits;
    bad.gaugeMeters=std::numeric_limits<double>::quiet_NaN();
    CHECK(!DesignRailCant(straight,bad).Valid());
    auto invalidStations=straight;
    invalidStations.stations[1].stationMeters=0;
    CHECK(!DesignRailCant(invalidStations).Valid());
    return 0;
}
