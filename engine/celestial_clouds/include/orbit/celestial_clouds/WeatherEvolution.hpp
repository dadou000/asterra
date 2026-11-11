#pragma once

#include <orbit/celestial_clouds/WeatherMultirateScheduler.hpp>
#include <orbit/celestial_clouds/StormZoneRuntime.hpp>
#include <orbit/celestial_clouds/WeatherCrossZoneField.hpp>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <future>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace orbit::celestial_clouds
{
// Experimental conservation-accounted *surrogate*, not a cloud-resolving solver.
// Quantities are normalized moisture inventory units, NOT SI mass or joules.
// It must never supply atmospheric heat/momentum tendencies or rendered storms.
struct ConvectiveChildState
{
    u64 id{0};
    f64 vapour{0.0};
    f64 condensate{0.0};
    f64 rain{0.0};
    f64 updraft{0.0};       // normalized intensity diagnostic, not m/s
    f64 eastKm{0.0};        // local position, relative to zone centre
    f64 northKm{0.0};
    f64 eastKmPerSecond{0.0};
    f64 northKmPerSecond{0.0};
    f64 coldPoolKm{0.0};
    f64 coldPoolStrength{0.0};  // normalized diagnostic, not thermodynamic deficit
    f64 externalLift{0.0};     // sampled from other zones for this step
    f64 externalUndercut{0.0}; // normalized, not SI forcing
};
struct WeatherZoneEvolutionState
{
    u64 zoneId{0};
    f64 simulatedSeconds{0.0};
    std::vector<ConvectiveChildState> children;
    f64 precipitationExported{0.0};
    // Emergent organization: component IDs and dwell times, not storm categories.
    struct Group { u64 id{0}; std::vector<u64> members; f64 coherentSeconds{0.0}; };
    std::vector<Group> groups;
};
struct WeatherExchangeRecord
{
    // Unique within one ledger epoch. Consumers must deduplicate by
    // (epoch, sequence) before acknowledging delivery.
    u64 epoch{0};
    u64 sequence{0};
    u64 zoneId{0};
    f64 fromSeconds{0.0};
    f64 toSeconds{0.0};
    f64 precipitation{0.0};  // exported normalized inventory only
};
struct WeatherEvolutionResult
{
    WeatherStepTicket ticket;
    WeatherZoneEvolutionState state;
    WeatherExchangeRecord exchange;
};
// A true atmospheric provider must implement this at a coupling barrier.
// Returning true means the exchange was accepted durably/idempotently;
// false means leave it in the outbox for retry. These quantities are still
// dimensionless; an SI atmospheric solver must not consume them directly.
class IWeatherExchangeConsumer
{
public:
    virtual ~IWeatherExchangeConsumer() = default;
    [[nodiscard]] virtual bool Accept(const WeatherExchangeRecord& exchange) = 0;
};

class WeatherConvectiveSurrogate
{
public:
    // Explicitly initialized when the zone becomes active; never materialize
    // a fully grown storm just because suitability is high.
    [[nodiscard]] static WeatherZoneEvolutionState Initial(const u64 zoneId, const f64 time)
    {
        WeatherZoneEvolutionState state;
        state.zoneId=zoneId;
        state.simulatedSeconds=time;
        // Three independent low-energy perturbations with deterministic IDs.
        for (u64 i=0; i<3; ++i)
        {
            ConvectiveChildState c;
            c.id=zoneId*4U+i+1U;
            c.vapour=1.0;
            c.eastKm=(static_cast<f64>(i)-1.0)*12.0;
            c.northKm=static_cast<f64>((zoneId+i)%3U)*5.0;
            c.eastKmPerSecond=(static_cast<f64>(i)-1.0)*0.001;
            c.updraft=0.01*(static_cast<f64>((zoneId+i*7U)%7U)+1.0);
            state.children.push_back(c);
        }
        return state;
    }

    // Only accepts explicitly validated short steps. Large timewarp requires
    // a separately validated long-step operator, never repeated O(warp) steps.
    [[nodiscard]] static std::optional<WeatherEvolutionResult> Evaluate(
        const WeatherZoneEvolutionState& original,
        const StormZoneWeights& environment,
        const WeatherStepTicket& ticket,
        const f64 maxDetailedSeconds=120.0)
    {
        const f64 dt=ticket.toSeconds-ticket.fromSeconds;
        if (ticket.zoneId!=original.zoneId ||
            ticket.kind!=WeatherStepKind::Detailed ||
            ticket.fromSeconds!=original.simulatedSeconds ||
            !std::isfinite(dt) || dt<=0.0 || dt>maxDetailedSeconds)
            return std::nullopt;

        WeatherEvolutionResult result;
        result.ticket=ticket;
        result.state=original;
        result.exchange.zoneId=original.zoneId;
        result.exchange.fromSeconds=ticket.fromSeconds;
        result.exchange.toSeconds=ticket.toSeconds;
        const f64 factor=std::clamp(static_cast<f64>(environment.convection),0.0,1.0);
        // Deterministic local partition: vapour -> condensate -> rain -> export.
        // No external water sources; never fabricate total inventory.
        for (auto& c:result.state.children)
        {
            if (!std::isfinite(c.vapour) || !std::isfinite(c.condensate) ||
                !std::isfinite(c.rain) || c.vapour<0 || c.condensate<0 || c.rain<0)
                return std::nullopt;
            const f64 growth=1.0-std::exp(-dt*factor*0.0008);
            const f64 condensation=std::min(c.vapour,c.vapour*growth);
            c.vapour-=condensation;
            c.condensate+=condensation;
            const f64 precipitation=c.condensate*(1.0-std::exp(-dt*0.0005));
            c.condensate-=precipitation;
            c.rain+=precipitation;
            const f64 exported=c.rain*(1.0-std::exp(-dt*0.0003));
            c.rain-=exported;
            result.exchange.precipitation+=exported;
            c.updraft=std::clamp(c.updraft+(factor*0.02-c.updraft*0.01)*dt/60.0,0.0,1.0);
        }
        result.state.simulatedSeconds=ticket.toSeconds;
        result.state.precipitationExported+=result.exchange.precipitation;
        return result;
    }

    [[nodiscard]] static f64 Inventory(const WeatherZoneEvolutionState& state)
    {
        f64 total=state.precipitationExported;
        for (const auto& c:state.children)
            total+=c.vapour+c.condensate+c.rain;
        return total;
    }
};

// Transactional completion: a zone state and its export record are committed
// together only if the associated scheduler ticket is still current.
class WeatherEvolutionLedger
{
public:
    void Reset()
    {
        states_.clear();
        records_.clear();
        // Invalidate receipts from the previous timeline or planetary body.
        ++epoch_;
        nextSequence_=1;
    }
    void Reconcile(const std::vector<StormZone>& zones, f64 atTime)
    {
        std::unordered_set<u64> active;
        for (const auto& z:zones)
        {
            if (z.id==0) continue;
            active.insert(z.id);
            states_.try_emplace(z.id,WeatherConvectiveSurrogate::Initial(z.id,atTime));
        }
        for (auto it=states_.begin();it!=states_.end();)
            if (!active.contains(it->first)) it=states_.erase(it);
            else ++it;
        // Keep exported records for a future real global-atmosphere consumer.
        // Records are immutable once inserted and not silently discarded.
    }

    [[nodiscard]] const WeatherZoneEvolutionState* State(u64 id) const
    {
        const auto it=states_.find(id);
        return it==states_.end()?nullptr:&it->second;
    }

    [[nodiscard]] bool HasExchangeCapacity() const noexcept {return records_.size()<4096U;}

    [[nodiscard]] bool Complete(WeatherMultirateScheduler& scheduler,
                               const WeatherEvolutionResult& result)
    {
        if (!HasExchangeCapacity()) return false;
        auto it=states_.find(result.ticket.zoneId);
        if (it==states_.end() ||
            it->second.simulatedSeconds!=result.ticket.fromSeconds ||
            result.state.simulatedSeconds!=result.ticket.toSeconds ||
            result.state.zoneId!=result.ticket.zoneId ||
            result.exchange.zoneId!=result.ticket.zoneId ||
            result.exchange.fromSeconds!=result.ticket.fromSeconds ||
            result.exchange.toSeconds!=result.ticket.toSeconds ||
            !std::isfinite(result.exchange.precipitation) ||
            result.exchange.precipitation<0 ||
            result.ticket.kind!=WeatherStepKind::Detailed)
            return false;
        // Validate the exported quantity against the committed state's
        // inventory delta before touching the scheduler or the existing state.
        const f64 before=WeatherConvectiveSurrogate::Inventory(it->second);
        const f64 after=WeatherConvectiveSurrogate::Inventory(result.state);
        const f64 exportedDelta=result.state.precipitationExported-
            it->second.precipitationExported;
        if (!std::isfinite(before) || !std::isfinite(after) ||
            !std::isfinite(exportedDelta) || exportedDelta<0.0 ||
            std::abs(after-before)>1.0e-9*std::max(1.0,std::abs(before)) ||
            std::abs(exportedDelta-result.exchange.precipitation)>
                1.0e-9*std::max(1.0,std::abs(exportedDelta)))
            return false;
        if (!scheduler.Commit(result.ticket)) return false;
        it->second=result.state;
        auto record=result.exchange;
        record.epoch=epoch_;
        record.sequence=nextSequence_++;
        records_.push_back(record);
        return true;
    }

    template<typename Visitor>
    void ForEachState(Visitor&& visitor) const
    {
        for(const auto& [id,state]:states_) { visitor(id,state); }
    }

    [[nodiscard]] const std::vector<WeatherExchangeRecord>& PendingExchanges() const
    {
        return records_;
    }

    // Call on the simulation owner thread at a verified coupling barrier.
    // Remove only the accepted prefix; a failed record and everything after
    // it stay pending in their original order. Budget bounds the delivery cost.
    [[nodiscard]] std::size_t Deliver(IWeatherExchangeConsumer& consumer,
                                      std::size_t maxRecords=64)
    {
        const std::size_t limit=std::min(records_.size(),maxRecords);
        std::size_t accepted=0;
        while (accepted<limit)
        {
            if (!consumer.Accept(records_[accepted])) break;
            ++accepted;
        }
        if (accepted!=0)
            records_.erase(records_.begin(),
                records_.begin()+static_cast<std::ptrdiff_t>(accepted));
        return accepted;
    }

    [[nodiscard]] u64 Epoch() const noexcept {return epoch_;}
    // No real atmospheric feedback consumer is connected yet.
private:
    std::unordered_map<u64,WeatherZoneEvolutionState> states_;
    std::vector<WeatherExchangeRecord> records_;
    u64 epoch_{1};
    u64 nextSequence_{1};
};
// One bounded background operation, shared across all zones. The result is
// published and reconciled only on the simulation owner thread. No GPU use.
class WeatherZoneEvolutionRunner
{
public:
    using EvolutionOperator = std::optional<WeatherEvolutionResult> (*)(
        const WeatherZoneEvolutionState&, const StormZoneWeights&, const WeatherStepTicket&);

    void SetEvaluator(EvolutionOperator evaluator) noexcept { evaluator_ = evaluator; }

    void Reset()
    {
        ++epoch_;
        scheduler_.Reset();
        ledger_.Reset(); // prototype inventory; do not treat as authoritative climate
        lastGeneration_=0;
        completed_=0;
        crossZone_.Clear();
    }
    void Tick(const StormZoneSnapshot& snapshot, f64 targetSeconds,
              f64 planetRadiusMeters)
    {
        if (!std::isfinite(targetSeconds)) return;
        if (snapshot.generation==0)
        {
            if(lastGeneration_!=0) Reset();
            return;
        }
        if (snapshot.generation!=lastGeneration_)
        {
            if (lastGeneration_!=0 && snapshot.simulatedSeconds<lastSnapshotTime_)
                Reset();
            scheduler_.Reconcile(snapshot.zones,snapshot.simulatedSeconds);
            ledger_.Reconcile(snapshot.zones,snapshot.simulatedSeconds);
            RefreshCrossZone(snapshot,planetRadiusMeters);
            lastGeneration_=snapshot.generation;
            lastSnapshotTime_=snapshot.simulatedSeconds;
        }
        if (worker_.valid())
        {
            if (worker_.wait_for(std::chrono::seconds(0))!=std::future_status::ready)
                return;
            auto finished=worker_.get();
            if (finished.epoch==epoch_ && finished.result &&
                ledger_.Complete(scheduler_,*finished.result))
            {
                ++completed_;
                RefreshCrossZone(snapshot,planetRadiusMeters);
            }
        }
        const auto tickets=scheduler_.Plan(targetSeconds);
        if (!ledger_.HasExchangeCapacity()) return;
        for (const auto& ticket:tickets)
        {
            if(ticket.kind!=WeatherStepKind::Detailed) continue;
            const auto* state=ledger_.State(ticket.zoneId);
            if(!state) continue;
            const auto it=std::find_if(snapshot.zones.begin(),snapshot.zones.end(),
                [&](const StormZone& z){return z.id==ticket.zoneId;});
            if(it==snapshot.zones.end()) continue;
            auto copy=*state;
            // Cross-zone perturbations are evaluated at the RECEIVER's own
            // committed time. No future reads, shared mutable state, or forced
            // synchronization between the two zones.
            for(auto& child:copy.children)
            {
                const auto location=WeatherLocalDirection(it->centre,
                    child.eastKm,child.northKm,planetRadiusMeters);
                const auto influence=crossZone_.Sample(ticket.zoneId,location,
                    ticket.fromSeconds,planetRadiusMeters);
                child.externalLift=influence.lift;
                child.externalUndercut=influence.undercut;
            }
            const auto weights=it->weights;
            const u64 jobEpoch=epoch_;
            const auto evaluator=evaluator_;
            worker_=std::async(std::launch::async,[ticket,copy,weights,jobEpoch,evaluator]
            {
                Job result;
                result.epoch=jobEpoch;
                result.result=evaluator ? evaluator(copy,weights,ticket)
                    : WeatherConvectiveSurrogate::Evaluate(copy,weights,ticket);
                return result;
            });
            break; // at most one outstanding shared worker, no per-zone threads
        }
    }
    [[nodiscard]] const WeatherMultirateScheduler& Scheduler() const noexcept {return scheduler_;}
    [[nodiscard]] const WeatherEvolutionLedger& Ledger() const noexcept {return ledger_;}
    [[nodiscard]] u64 Completed() const noexcept {return completed_;}
    [[nodiscard]] std::size_t CrossZoneSources() const noexcept
    { return crossZone_.SourceCount(); }
private:
    void RefreshCrossZone(const StormZoneSnapshot& snapshot,
                          f64 planetRadiusMeters)
    {
        if(!std::isfinite(planetRadiusMeters) || planetRadiusMeters<=0)
        {
            crossZone_.Clear();
            return;
        }
        std::vector<WeatherCrossZoneSource> sources;
        ledger_.ForEachState([&](u64 id,const WeatherZoneEvolutionState& state)
        {
            const auto zone=std::find_if(snapshot.zones.begin(),snapshot.zones.end(),
                [&](const StormZone& z){ return z.id==id; });
            if(zone==snapshot.zones.end()) return;
            for(const auto& child:state.children)
            {
                if(child.coldPoolKm<=0.001 || child.coldPoolStrength<=0.001)
                    continue;
                sources.push_back({
                    id,child.id,WeatherLocalDirection(zone->centre,
                        child.eastKm,child.northKm,planetRadiusMeters),
                    state.simulatedSeconds,child.coldPoolKm,child.coldPoolStrength});
            }
        });
        crossZone_.Publish(std::move(sources));
    }

    struct Job
    {
        u64 epoch{0};
        std::optional<WeatherEvolutionResult> result;
    };
    EvolutionOperator evaluator_{nullptr};
    WeatherMultirateScheduler scheduler_{};
    WeatherEvolutionLedger ledger_{};
    WeatherCrossZoneField crossZone_{};
    std::future<Job> worker_{};
    u64 epoch_{0};
    u64 lastGeneration_{0};
    u64 completed_{0};
    f64 lastSnapshotTime_{0};
};
} // namespace orbit::celestial_clouds
