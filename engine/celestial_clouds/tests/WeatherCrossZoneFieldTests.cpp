#include <orbit/celestial_clouds/WeatherCrossZoneField.hpp>
#include <orbit/celestial_clouds/WeatherEmergence.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

using namespace orbit;
using namespace orbit::celestial_clouds;
namespace {
int failures=0;
void Check(bool value, const char* what)
{
    if(!value) {++failures;std::printf("FAIL: %s\n",what);}
}
}
int main()
{
    constexpr f64 radius=6371000.0;
    const math::Double3 centre{1.0,0.0,0.0};
    const auto moved=WeatherLocalDirection(centre,100,0,radius);
    Check(std::abs(std::acos(StormZoneDot(centre,moved))*radius*0.001-100)<1e-6,
        "local tangent mapping has correct arc distance");
    const auto seam=WeatherLocalDirection({-1.0,0.0,0.0},20,0,radius);
    Check(std::isfinite(seam.x)&&std::isfinite(seam.z),"tangent mapping crosses dateline");
    const auto northPole=WeatherLocalDirection({0.0,1.0,0.0},10,10,radius);
    Check(std::isfinite(northPole.x)&&std::isfinite(northPole.y),
        "tangent mapping remains finite at pole");

    WeatherCrossZoneField field;
    WeatherCrossZoneSource emitter;
    emitter.zoneId=1;
    emitter.childId=5;
    emitter.direction=centre;
    emitter.radiusKm=20;
    emitter.strength=0.8;
    emitter.sourceSeconds=100;
    field.Publish({emitter,emitter});
    Check(field.SourceCount()==1,"same child sources deduplicated");
    const auto edge=WeatherLocalDirection(centre,20,0,radius);
    const auto lift=field.Sample(2,edge,100,radius);
    Check(lift.lift>0.75 && lift.undercut==0 && lift.contributingSources==1,
        "neighbor zone receives edge lifting");
    const auto inside=field.Sample(2,centre,100,radius);
    Check(inside.undercut>0.75 && inside.lift<0.01,
        "neighbor zone receives interior suppression");
    Check(field.Sample(1,edge,100,radius).contributingSources==0,
        "source does not affect its own zone twice");
    Check(field.Sample(2,edge,99,radius).contributingSources==0,
        "future source not readable");
    Check(field.Sample(2,edge,1001,radius).contributingSources==0,
        "stale source expires");
    const auto fading=field.Sample(2,edge,500,radius);
    Check(fading.lift>0 && fading.lift<lift.lift,
        "influence decays with source age");
    Check(field.Sample(2,WeatherLocalDirection(centre,250,0,radius),100,radius)
          .contributingSources==0,"distant cells not coupled");

    WeatherZoneEvolutionState state;
    state.zoneId=2;
    ConvectiveChildState cell;
    cell.id=10;cell.vapour=1;cell.updraft=0.5;
    cell.externalLift=lift.lift;
    state.children.push_back(cell);
    auto baseline=state;
    baseline.children[0].externalLift=0;
    const f64 inventory=WeatherConvectiveSurrogate::Inventory(state);
    Check(WeatherEmergence::Advance(state,60),"remote lifting advances diagnostic");
    Check(WeatherEmergence::Advance(baseline,60),"unforced control advances");
    Check(state.children[0].updraft>baseline.children[0].updraft,
        "external cross-zone influence modifies next detailed step");
    Check(std::abs(WeatherConvectiveSurrogate::Inventory(state)-inventory)<1e-12,
        "cross-zone diagnostic does not synthesize inventory");
    field.Clear();
    Check(field.SourceCount()==0,"field clears on body change or rewind");
    Check(failures==0,"all cross-zone tests");
    return failures?1:0;
}
