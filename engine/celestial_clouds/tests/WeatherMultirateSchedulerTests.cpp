#include <orbit/celestial_clouds/WeatherMultirateScheduler.hpp>
#include <cstdio>
using namespace orbit;
using namespace orbit::celestial_clouds;
namespace {
int errors=0;
void Check(bool pass,const char* name){if(!pass){++errors;std::printf("FAIL: %s\n",name);}}
StormZone Zone(u64 id,float activity)
{
    StormZone z; z.id=id; z.weights.convection=activity; return z;
}
}
int main()
{
    WeatherStepBudget budget;
    budget.maxTickets=2;
    budget.maxEstimatedCost=2;
    budget.maximumDetailedStep=120;
    WeatherMultirateScheduler scheduler(budget);
    scheduler.Reconcile({Zone(9,1),Zone(2,0),Zone(4,1)},0);
    Check(scheduler.NodeCount()==3,"reconcile three nodes");
    const auto first=scheduler.Plan(60);
    Check(first.size()==2,"budget caps issued tickets");
    Check(scheduler.Stats().deferred==1,"remaining work deferred, not queued");
    Check(first[0].zoneId<first[1].zoneId,"stable tie ordering");
    Check(first[0].kind==WeatherStepKind::Detailed,"short work requests detailed operator");
    const auto old=scheduler.Plan(60);
    Check(old.size()==2 && old[0].fromSeconds==0,"planning alone cannot advance physics");
    Check(scheduler.Commit(first[0]),"successful real completion accepted");
    Check(!scheduler.Commit(first[0]),"stale duplicate completion rejected");
    const auto later=scheduler.Plan(86400000);
    Check(later.size()==2,"huge warp has bounded ticket count");
    Check(later[0].kind==WeatherStepKind::RequiresLongStepModel,"long warp requires compatible backend");
    Check(later[0].toSeconds==86400000,"one long ticket spans entire backlog");
    Check(scheduler.Stats().blockedLongSteps==2,"long-step obligations visible");
    Check(!scheduler.Commit({999,0,1,WeatherStepKind::Detailed,1}),"unknown node rejected");
    scheduler.Reconcile({Zone(9,1)},86400000);
    Check(scheduler.NodeCount()==1,"retired nodes removed");
    scheduler.Reconcile({Zone(9,1)},10);
    Check(scheduler.NodeCount()==1,"rewind resets scheduler");
    const auto reverse=scheduler.Plan(70);
    Check(!reverse.empty() && reverse.front().fromSeconds==10,"rewind restarts at requested time");
    Check(errors==0,"all checks passed");
    return errors?1:0;
}
