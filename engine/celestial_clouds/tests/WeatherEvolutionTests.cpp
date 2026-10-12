#include <orbit/celestial_clouds/WeatherEvolution.hpp>
#include <cmath>
#include <cstdio>
using namespace orbit;
using namespace orbit::celestial_clouds;
namespace {
int failures=0;
void Check(bool value,const char* label)
{
    if (!value){++failures;std::printf("FAIL: %s\n",label);}
}
StormZone Zone()
{
    StormZone z;z.id=42;z.weights.convection=0.9F;return z;
}
}
int main()
{
    WeatherStepBudget budget;
    budget.maximumDetailedStep=120;
    WeatherMultirateScheduler scheduler(budget);
    WeatherEvolutionLedger ledger;
    auto zone=Zone();
    scheduler.Reconcile({zone},0);
    ledger.Reconcile({zone},0);
    auto first=scheduler.Plan(60);
    Check(first.size()==1,"one due zone");
    auto state=ledger.State(42);
    Check(state!=nullptr && state->children.size()==3,"three independent seed children");
    const auto starting=WeatherConvectiveSurrogate::Inventory(*state);
    const auto calculated=WeatherConvectiveSurrogate::Evaluate(*state,zone.weights,first.front());
    Check(calculated.has_value(),"detailed step evaluated");
    if(calculated)
    {
        Check(std::abs(WeatherConvectiveSurrogate::Inventory(calculated->state)-starting)<1e-10,
            "inventory conserved including exported rain");
        Check(ledger.Complete(scheduler,*calculated),"transaction completes");
        Check(!ledger.Complete(scheduler,*calculated),"duplicate transaction rejected");
        Check(ledger.PendingExchanges().size()==1,"one export ledger record");
        Check(ledger.State(42)->simulatedSeconds==60,"state advances only on completion");
    }
    const auto large=scheduler.Plan(1e8);
    Check(large.size()==1 && large[0].kind==WeatherStepKind::RequiresLongStepModel,
        "extreme timewarp is a long-step obligation");
    Check(!WeatherConvectiveSurrogate::Evaluate(*ledger.State(42),zone.weights,large[0]).has_value(),
        "detailed operator refuses extreme timewarp");
    Check(ledger.State(42)->simulatedSeconds==60,
        "long-step rejection does not forge progress");
    WeatherStepTicket stale{42,60,180,WeatherStepKind::Detailed,1};
    auto maybe=WeatherConvectiveSurrogate::Evaluate(*ledger.State(42),zone.weights,stale);
    Check(maybe.has_value(),"boundary-length step accepted");
    if(maybe)
    {
        maybe->state.children[0].vapour+=1.0;
        Check(!ledger.Complete(scheduler,*maybe),"nonconservative completion rejected");
        maybe->state.children[0].vapour-=1.0;
        maybe->exchange.toSeconds=999;
        Check(!ledger.Complete(scheduler,*maybe),"mismatched flux window rejected");
        Check(ledger.State(42)->simulatedSeconds==60,"invalid transaction has no side effects");
    }
    ledger.Reconcile({},200);
    Check(ledger.State(42)==nullptr,"dissolved zone state retired");
    Check(ledger.PendingExchanges().size()==1,"unconsumed exchange retained");

    // Delivery is strictly ordered and never removes a rejected record.
    struct Consumer final : IWeatherExchangeConsumer
    {
        std::vector<std::pair<u64,u64>> received;
        bool reject{false};
        [[nodiscard]] bool Accept(const WeatherExchangeRecord& r) override
        {
            if(reject) return false;
            received.emplace_back(r.epoch,r.sequence);
            return true;
        }
    } consumer;
    Check(ledger.PendingExchanges().size()==1,"retired zone retains undelivered export");
    const auto epoch=ledger.PendingExchanges().front().epoch;
    const auto sequence=ledger.PendingExchanges().front().sequence;
    consumer.reject=true;
    Check(ledger.Deliver(consumer)==0,"consumer rejection retains export");
    Check(ledger.PendingExchanges().size()==1,"rejected record stays pending");
    consumer.reject=false;
    Check(ledger.Deliver(consumer,0)==0,"zero work budget delivers nothing");
    Check(ledger.Deliver(consumer,1)==1,"acknowledged record can be removed");
    Check(consumer.received.size()==1 &&
          consumer.received[0]==std::make_pair(epoch,sequence),
          "consumer receives stable epoch and sequence");
    Check(ledger.PendingExchanges().empty(),"acknowledgment drains outbox");
    Check(ledger.Deliver(consumer)==0,"completed receipt never redelivered");

    // Two independent exports: failure at the second record must preserve it
    // without replaying the already acknowledged first record.
    WeatherMultirateScheduler orderedScheduler(budget);
    WeatherEvolutionLedger orderedLedger;
    orderedScheduler.Reconcile({zone},0);
    orderedLedger.Reconcile({zone},0);
    for (int i=0;i<2;++i)
    {
        const f64 target=i==0 ? 60.0 : 180.0;
        const auto tickets=orderedScheduler.Plan(target);
        Check(tickets.size()==1,"ordered ledger produces a work ticket");
        if(tickets.empty()) break;
        const auto calculatedStep=WeatherConvectiveSurrogate::Evaluate(
            *orderedLedger.State(42),zone.weights,tickets.front());
        Check(calculatedStep.has_value(),"ordered export step evaluates");
        if(calculatedStep)
            Check(orderedLedger.Complete(orderedScheduler,*calculatedStep),
                "ordered export is committed");
    }
    struct RejectSecond final : IWeatherExchangeConsumer
    {
        u32 accepts{0};
        bool blockSecond{true};
        [[nodiscard]] bool Accept(const WeatherExchangeRecord&) override
        {
            if(blockSecond && accepts==1) return false;
            ++accepts;
            return true;
        }
    } rejectSecond;
    Check(orderedLedger.PendingExchanges().size()==2,"two exports queued");
    Check(orderedLedger.Deliver(rejectSecond,2)==1,"only acknowledged prefix removed");
    Check(orderedLedger.PendingExchanges().size()==1 &&
          orderedLedger.PendingExchanges().front().sequence==2,
          "second export remains in order");
    rejectSecond.blockSecond=false;
    Check(orderedLedger.Deliver(rejectSecond)==1 && rejectSecond.accepts==2,
        "retry consumes only second record");

    ledger.Reset();
    Check(ledger.Epoch()!=epoch,"reset invalidates former timeline receipts");
    Check(failures==0,"all tests");
    return failures?1:0;
}
