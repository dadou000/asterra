#pragma once

#include <orbit/celestial_clouds/AnalyticZoneSurvey.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace orbit::celestial_clouds
{
// Physics-neutral scheduler for the sparse discovery layer. No render-thread
// blocking, per-frame global weight arrays, or one-thread-per-zone ownership.
struct StormZoneRuntimeConfig
{
    f64 surveyIntervalSeconds{300.0};
    AnalyticZoneSurveyConfig survey{};
    StormZoneConfig zones{};
};

struct StormZoneSnapshot
{
    f64 simulatedSeconds{0.0};
    u64 generation{0};
    u32 candidates{0};
    std::vector<StormZone> zones;
};

class StormZoneRuntime
{
public:
    explicit StormZoneRuntime(StormZoneRuntimeConfig config={})
        : config_(config), discovery_(config.zones),
          snapshot_(std::make_shared<const StormZoneSnapshot>()) {}

    // Tick with the authoritative planetary clock and body identity.
    // A nonblocking poll; only a completed worker result can change the snapshot.
    // The caller may invoke Tick on each frame; costly work is cadence-gated.
    void Tick(u64 bodyId, f64 simulatedSeconds, WeatherParameters weather, WeatherClimate climate)
    {
        if (!std::isfinite(simulatedSeconds)) { return; }
        if (bodyId_!=bodyId || simulatedSeconds<lastRequestedTime_)
        {
            // An older job cannot be cancelled; it will be discarded on completion.
            ++epoch_;
            bodyId_=bodyId;
            discovery_=SparseStormZones(config_.zones);
            lastRequestedTime_=-1.0e300;
            lastPublishedTime_=-1.0e300;
            snapshot_=std::make_shared<const StormZoneSnapshot>();
        }
        if (worker_.valid() &&
            worker_.wait_for(std::chrono::seconds(0))==std::future_status::ready)
        {
            const auto result=worker_.get();
            if (result.epoch==epoch_ && result.bodyId==bodyId_)
            {
                discovery_.Discover(result.probes,result.time);
                auto output=std::make_shared<StormZoneSnapshot>();
                output->simulatedSeconds=result.time;
                output->generation=++generation_;
                output->candidates=static_cast<u32>(result.probes.size());
                output->zones=discovery_.Zones();
                snapshot_=std::move(output);
                lastPublishedTime_=result.time;
            }
        }
        if (worker_.valid()) { return; }
        const f64 interval=std::max(1.0,config_.surveyIntervalSeconds);
        if (lastRequestedTime_>-1.0e299 && simulatedSeconds-lastRequestedTime_<interval)
        {
            return;
        }
        const u64 requestedEpoch=epoch_;
        const u64 requestedBody=bodyId;
        const auto surveyConfig=config_.survey;
        lastRequestedTime_=simulatedSeconds;
        worker_=std::async(std::launch::async,
            [requestedEpoch,requestedBody,simulatedSeconds,weather,climate,surveyConfig]
            {
                Result result;
                result.epoch=requestedEpoch;
                result.bodyId=requestedBody;
                result.time=simulatedSeconds;
                result.probes=SurveyAnalyticStormZones(
                    simulatedSeconds,weather,climate,surveyConfig);
                return result;
            });
    }

    // Intended to be called by a single orchestration thread. Returned snapshots
    // remain immutable and safe to retain across future updates.
    [[nodiscard]] std::shared_ptr<const StormZoneSnapshot> Snapshot() const noexcept
    {
        return snapshot_;
    }

    [[nodiscard]] bool Busy() const
    {
        return worker_.valid() &&
            worker_.wait_for(std::chrono::seconds(0))!=std::future_status::ready;
    }

private:
    struct Result
    {
        u64 epoch{0};
        u64 bodyId{0};
        f64 time{0};
        std::vector<StormZoneProbe> probes;
    };

    StormZoneRuntimeConfig config_;
    SparseStormZones discovery_;
    std::future<Result> worker_;
    std::shared_ptr<const StormZoneSnapshot> snapshot_;
    u64 bodyId_{~0ULL};
    u64 epoch_{0};
    u64 generation_{0};
    f64 lastRequestedTime_{-1.0e300};
    f64 lastPublishedTime_{-1.0e300};
};
} // namespace orbit::celestial_clouds
