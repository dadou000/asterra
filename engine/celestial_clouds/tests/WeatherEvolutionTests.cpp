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
        maybe->exchange.toSeconds=999;
        Check(!ledger.Complete(scheduler,*maybe),"mismatched flux window rejected");
        Check(ledger.State(42)->simulatedSeconds==60,"invalid transaction has no side effects");
    }
    ledger.Reconcile({},200);
    Check(ledger.State(42)==nullptr,"dissolved zone state retired");
    Check(ledger.PendingExchanges().size()==1,"unconsumed exchange retained");
    Check(failures==0,"all tests");
    return failures?1:0;
}
