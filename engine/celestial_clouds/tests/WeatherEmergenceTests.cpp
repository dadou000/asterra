#include <orbit/celestial_clouds/WeatherEmergence.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
using namespace orbit;
using namespace orbit::celestial_clouds;
namespace {
int fails=0;
void Check(bool ok,const char* what) {if(!ok){++fails;std::printf("FAIL: %s\n",what);}}
WeatherZoneEvolutionState Fixture()
{
    WeatherZoneEvolutionState s;
    s.zoneId=7;
    s.children={
        {.id=10,.vapour=1,.updraft=.8,.eastKm=0,.northKm=0,
         .eastKmPerSecond=.001,.coldPoolKm=10,.coldPoolStrength=.8},
        {.id=11,.vapour=2,.updraft=.7,.eastKm=9,.northKm=0},
        {.id=12,.vapour=3,.updraft=.3,.eastKm=120,.northKm=0}
    };
    return s;
}
}
int main()
{
    auto s=Fixture();
    const f64 inventory=WeatherConvectiveSurrogate::Inventory(s);
    Check(WeatherEmergence::Advance(s,60),"valid weather step");
    Check(s.children[0].eastKm>0.05,"cell advects locally");
    Check(s.children[0].coldPoolKm>=10,"cold pool footprint expands");
    Check(s.children[1].updraft>.7,"cold-pool edge reinforces neighbor");
    Check(s.groups.size()==1 && s.groups[0].members.size()==2,
        "two nearby cells form potential organizational group");
    Check(WeatherEmergence::OrganizedGroups(s)==0,"one contact does not imply organization");
    const auto originalId=s.groups[0].id;
    for(int i=0;i<4;++i)
        Check(WeatherEmergence::Advance(s,60),"subsequent detailed step");
    Check(WeatherEmergence::OrganizedGroups(s)==1,"sustained group becomes organized");
    Check(s.groups[0].id==originalId,"persistent membership retains group ID");
    Check(std::abs(WeatherConvectiveSurrogate::Inventory(s)-inventory)<1e-12,
        "cold-pool feedback does not fabricate water inventory");
    s.children[1].eastKm=60;
    Check(WeatherEmergence::Advance(s,60),"splitting geometry advances");
    Check(s.groups.empty(),"disconnected cells dissolve parent group");
    const auto old=s;
    Check(!WeatherEmergence::Advance(s,3600),"large timestep rejected");
    Check(s.children[0].eastKm==old.children[0].eastKm,
        "invalid step cannot mutate state");
    auto bad=Fixture();
    bad.children[0].coldPoolKm=-1;
    Check(!WeatherEmergence::Advance(bad,30),"invalid cold pool rejected");
    auto isolated=Fixture();
    isolated.children[1].eastKm=65;
    isolated.children[2].eastKm=130;
    for(int i=0;i<6;++i)
        Check(WeatherEmergence::Advance(isolated,60),"isolated environment evolves");
    Check(WeatherEmergence::OrganizedGroups(isolated)==0,
        "favorable intensity without interaction does not force organized storms");

    const auto initial=WeatherConvectiveSurrogate::Initial(7,0);
    WeatherStepTicket ticket{7,0,60,WeatherStepKind::Detailed,1};
    StormZoneWeights weights;weights.convection=.9F;
    auto result=EvaluateEmergentWeather(initial,weights,ticket);
    Check(result.has_value(),"full emergent operator executes detailed step");
    if(result)
        Check(std::abs(WeatherConvectiveSurrogate::Inventory(initial)-
                       WeatherConvectiveSurrogate::Inventory(result->state))<1e-9,
              "entire pipeline conserves normalized inventory");
    ticket.kind=WeatherStepKind::RequiresLongStepModel;
    Check(!EvaluateEmergentWeather(initial,weights,ticket).has_value(),
        "unvalidated long step stays blocked");
    Check(fails==0,"all checks");
    return fails?1:0;
}
