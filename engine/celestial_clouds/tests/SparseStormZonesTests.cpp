#include <orbit/celestial_clouds/SparseStormZones.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

using namespace orbit;
using namespace orbit::celestial_clouds;

namespace {
int failures=0;
void Check(bool condition, const char* name)
{
    if (!condition) { std::printf("FAIL: %s\n", name); ++failures; }
}
StormZoneProbe Probe(f64 angle, f32 score)
{
    return {{std::cos(angle), 0.0, std::sin(angle)}, score, 0.8F, 0.9F, 0.3F};
}
}

int main()
{
    StormZoneConfig cfg;
    cfg.enterThreshold=0.6F;
    cfg.retainThreshold=0.4F;
    cfg.neighbourRadians=0.05;
    cfg.matchRadians=0.15;
    cfg.retireAfterSeconds=120.0;
    SparseStormZones zones(cfg);
    zones.Discover({Probe(0.0,0.9F),Probe(0.02,0.8F),Probe(1.0,0.85F),Probe(2.0,0.1F)},10.0);
    Check(zones.Zones().size()==2,"two sparse regions, low opportunity ignored");
    if (zones.Zones().size()==2)
    {
        const auto id=zones.Zones().front().id;
        Check(zones.Zones().front().contributingProbes==2,"nearby probes cluster");
        Check(zones.Zones().front().weights.initiation>=0.8F,"zone receives aggregate weight");
        zones.Discover({Probe(1.0,0.88F),Probe(0.0,0.83F),Probe(0.02,0.7F)},20.0);
        Check(zones.Zones().size()==2 && zones.Zones().front().id==id,
            "probe ordering changes do not change matching identity");
    }
    zones.Discover({},60.0);
    Check(zones.Zones().size()==2,"temporary lull retains zones");
    zones.Discover({},141.0);
    Check(zones.Zones().empty(),"expired zones retire");

    SparseStormZones none(cfg);
    none.Discover({Probe(0.0,0.55F),Probe(0.01,0.54F)},10.0);
    Check(none.Zones().empty(),"non-triggering region does not spawn a zone");
    none.Discover({Probe(0.0,0.8F)},20.0);
    const u64 stable=none.Zones().front().id;
    none.Discover({Probe(0.0,0.49F)},30.0);
    Check(none.Zones().size()==1 && none.Zones().front().id==stable,
        "hysteresis allows active region to remain");
    none.Discover({Probe(0.0,0.9F)},5.0);
    Check(none.Zones().size()==1,"backward time resets discovery history");

    const auto p=AnalyticStormProbe({1,0,0},1000.0,WeatherParameters{},WeatherClimate{});
    Check(std::isfinite(p.opportunity) && p.opportunity>=0 && p.opportunity<=1,
        "analytic bridge has bounded screening value");
    Check(failures==0,"all zone tests");
    return failures==0?0:1;
}
