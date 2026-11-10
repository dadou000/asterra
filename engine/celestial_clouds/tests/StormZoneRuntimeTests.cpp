#include <orbit/celestial_clouds/StormZoneRuntime.hpp>

#include <chrono>
#include <cstdio>
#include <thread>

using namespace orbit;
using namespace orbit::celestial_clouds;

namespace {
int failures=0;
void Check(bool ok,const char* message)
{
    if (!ok) { std::printf("FAIL: %s\n",message); ++failures; }
}
bool Drain(StormZoneRuntime& runtime,u64 body,f64 time,WeatherParameters weather,
           WeatherClimate climate)
{
    for (int i=0;i<1500;++i)
    {
        runtime.Tick(body,time,weather,climate);
        if (runtime.Snapshot()->generation!=0 && !runtime.Busy())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}
}
int main()
{
    StormZoneRuntimeConfig cfg;
    cfg.surveyIntervalSeconds=300;
    cfg.survey.samples=128;
    cfg.survey.maxCandidates=64;
    cfg.survey.minimumOpportunity=0;
    cfg.zones.enterThreshold=0.0F;
    cfg.zones.neighbourRadians=0.5;
    cfg.zones.matchRadians=0.7;
    WeatherParameters weather;
    WeatherClimate climate;
    StormZoneRuntime runtime(cfg);
    const auto zero=runtime.Snapshot();
    Check(zero->generation==0,"empty snapshot at startup");
    Check(Drain(runtime,11,1000,weather,climate),"initial asynchronous scan completes");
    const auto first=runtime.Snapshot();
    Check(first->simulatedSeconds==1000,"published time is authoritative");
    Check(first->candidates<=64,"candidate budget respected");
    runtime.Tick(11,1100,weather,climate);
    Check(runtime.Snapshot()->generation==first->generation,"cadence prevents redundant scan");
    Check(zero->generation==0,"old immutable snapshot remains unchanged");
    runtime.Tick(11,1400,weather,climate);
    for (int i=0;i<1500 && runtime.Snapshot()->generation==first->generation;++i)
    {
        runtime.Tick(11,1400,weather,climate);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Check(runtime.Snapshot()->generation>first->generation,"cadence schedules another scan");
    const auto previous=runtime.Snapshot();
    runtime.Tick(12,1500,weather,climate);
    Check(runtime.Snapshot()->generation==0,"body switch invalidates old snapshot");
    Check(Drain(runtime,12,1500,weather,climate),"new body scan completes");
    runtime.Tick(12,500,weather,climate);
    Check(runtime.Snapshot()->generation==0,"reverse time invalidates future snapshot");
    Check(Drain(runtime,12,500,weather,climate),"reverse time rebuilds zone state");
    Check(previous->simulatedSeconds==1400,"retained snapshot keeps its original timestamp");
    Check(failures==0,"runtime checks");
    return failures?1:0;
}
