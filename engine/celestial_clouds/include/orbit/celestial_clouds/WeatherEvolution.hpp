#pragma once

#include <orbit/celestial_clouds/WeatherMultirateScheduler.hpp>

#include <algorithm>
#include <cmath>
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
};
struct WeatherZoneEvolutionState
{
    u64 zoneId{0};
    f64 simulatedSeconds{0.0};
    std::vector<ConvectiveChildState> children;
    f64 precipitationExported{0.0};
};
struct WeatherExchangeRecord
{
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

    [[nodiscard]] bool Complete(WeatherMultirateScheduler& scheduler,
                               const WeatherEvolutionResult& result)
    {
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
        if (!scheduler.Commit(result.ticket)) return false;
        it->second=result.state;
        records_.push_back(result.exchange);
        return true;
    }

    [[nodiscard]] const std::vector<WeatherExchangeRecord>& PendingExchanges() const
    {
        return records_;
    }
    // No global feedback sink exists yet. Records are intentionally retained.
private:
    std::unordered_map<u64,WeatherZoneEvolutionState> states_;
    std::vector<WeatherExchangeRecord> records_;
};
} // namespace orbit::celestial_clouds
