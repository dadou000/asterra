#pragma once
#include <orbit/celestial_clouds/SparseStormZones.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace orbit::celestial_clouds
{
// Phase 4: bounded multirate work *planning*. This class never advances fluid
// state. A long-step ticket is only an obligation for a validated model, not
// permission to skip precipitation, circulation or conservation exchanges.
enum class WeatherStepKind : u8 { Detailed, RequiresLongStepModel };
struct WeatherStepTicket
{
    u64 zoneId{0};
    f64 fromSeconds{0};
    f64 toSeconds{0};
    WeatherStepKind kind{WeatherStepKind::Detailed};
    f32 estimatedCost{1.0F};
};
struct WeatherStepBudget
{
    u32 maxTickets{8};
    f32 maxEstimatedCost{8.0F}; // scheduling work units, NOT wall-clock milliseconds
    f64 maximumDetailedStep{120.0};
    f64 minimumZoneInterval{15.0};
    f64 maximumZoneInterval{900.0};
};
struct WeatherStepStats
{
    u32 activeZones{0};
    u32 issued{0};
    u32 blockedLongSteps{0};
    u32 deferred{0};
    f64 maximumLagSeconds{0.0};
};
class WeatherMultirateScheduler
{
public:
    explicit WeatherMultirateScheduler(WeatherStepBudget budget={}) : budget_(budget) {}

    void Reset()
    {
        nodes_.clear();
        lastTime_=-std::numeric_limits<f64>::infinity();
        stats_={};
    }
    void Reconcile(const std::vector<StormZone>& zones, const f64 now)
    {
        if (!std::isfinite(now)) return;
        if (now<lastTime_) Reset();
        lastTime_=now;
        std::unordered_set<u64> live;
        live.reserve(zones.size());
        for (const auto& z: zones)
        {
            if (z.id==0) continue;
            live.insert(z.id);
            auto [it,inserted]=nodes_.try_emplace(z.id);
            auto& node=it->second;
            if (inserted)
            {
                node.committed=now; // newly discovered: no invented past history
                node.nextDue=now;   // initial scheduling opportunity
            }
            // Strongly convective regions get more frequent diagnostic work.
            // This is a cadence heuristic only, not a physical timestep bound.
            const f64 activity=std::clamp(static_cast<f64>(z.weights.convection),0.0,1.0);
            node.interval=std::clamp(
                budget_.maximumZoneInterval*(1.0-0.92*activity),
                budget_.minimumZoneInterval,budget_.maximumZoneInterval);
            node.cost=1.0F;
        }
        for (auto it=nodes_.begin();it!=nodes_.end();)
        {
            if (!live.contains(it->first)) it=nodes_.erase(it);
            else ++it;
        }
    }
    // Plan at most one ticket per due node. Backlog is represented by a
    // single long-step obligation; never enqueue O(timewarp) old ticks.
    [[nodiscard]] std::vector<WeatherStepTicket> Plan(f64 target)
    {
        std::vector<WeatherStepTicket> tickets;
        stats_={};
        stats_.activeZones=static_cast<u32>(nodes_.size());
        if (!std::isfinite(target) || target<lastTime_)
            return tickets;
        std::vector<u64> due;
        due.reserve(nodes_.size());
        for (const auto& [id,node]:nodes_)
        {
            if (target>=node.nextDue && target>node.committed)
                due.push_back(id);
            stats_.maximumLagSeconds=std::max(stats_.maximumLagSeconds,
                std::max(0.0,target-node.committed));
        }
        // Deterministic priority: most overdue first, then stable ID.
        std::sort(due.begin(),due.end(),[&](u64 a,u64 b)
        {
            const f64 lagA=target-nodes_.at(a).nextDue;
            const f64 lagB=target-nodes_.at(b).nextDue;
            return lagA==lagB ? a<b : lagA>lagB;
        });
        f32 cost=0;
        for (u64 id:due)
        {
            auto& node=nodes_.at(id);
            if (tickets.size()>=budget_.maxTickets ||
                cost+node.cost>budget_.maxEstimatedCost)
            {
                ++stats_.deferred;
                continue;
            }
            const f64 advance=std::min(target,node.committed+node.interval);
            const bool longStep=target-node.committed>
                std::max(1.0,budget_.maximumDetailedStep);
            // For an extreme jump, ticket requests the whole missing interval.
            // Until a real long-step backend exists, this remains blocked.
            const f64 end=longStep ? target : advance;
            tickets.push_back({id,node.committed,end,
                longStep?WeatherStepKind::RequiresLongStepModel:WeatherStepKind::Detailed,
                node.cost});
            ++stats_.issued;
            if (longStep) ++stats_.blockedLongSteps;
            cost+=node.cost;
        }
        return tickets;
    }
    // Called only after a real operator completes the work and its exchanges
    // are reconciled. Merely issuing a ticket does NOT move simulated state.
    [[nodiscard]] bool Commit(const WeatherStepTicket& ticket)
    {
        const auto it=nodes_.find(ticket.zoneId);
        if (it==nodes_.end()) return false;
        auto& node=it->second;
        if (!std::isfinite(ticket.toSeconds) ||
            ticket.fromSeconds!=node.committed ||
            ticket.toSeconds<=ticket.fromSeconds)
            return false;
        node.committed=ticket.toSeconds;
        node.nextDue=node.committed+node.interval;
        return true;
    }
    [[nodiscard]] const WeatherStepStats& Stats() const noexcept {return stats_;}
    [[nodiscard]] std::size_t NodeCount() const noexcept {return nodes_.size();}
private:
    struct Node
    {
        f64 committed{0};
        f64 nextDue{0};
        f64 interval{300};
        f32 cost{1};
    };
    WeatherStepBudget budget_;
    std::unordered_map<u64,Node> nodes_;
    f64 lastTime_{-std::numeric_limits<f64>::infinity()};
    WeatherStepStats stats_;
};
} // namespace orbit::celestial_clouds
