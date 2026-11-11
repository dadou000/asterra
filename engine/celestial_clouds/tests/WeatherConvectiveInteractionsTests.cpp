#include <orbit/celestial_clouds/WeatherConvectiveInteractions.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace orbit;
using namespace orbit::celestial_clouds;

namespace
{
int failures=0;
void Check(bool ok,const char* label)
{
    if(!ok){++failures;std::printf("FAIL: %s\n",label);}
}
WeatherZoneEvolutionState Three()
{
    WeatherZoneEvolutionState state;
    state.zoneId=77;
    state.children={
        {.id=1,.vapour=1.0,.updraft=0.6,.eastKm=0,.northKm=0},
        {.id=2,.vapour=3.0,.updraft=0.8,.eastKm=8,.northKm=0},
        {.id=3,.vapour=2.0,.updraft=0.2,.eastKm=150,.northKm=0}};
    return state;
}
}
int main()
{
    auto state=Three();
    const auto graph=WeatherConvectiveInteractions::Build(state);
    Check(graph.edges.size()==1,"only nearby pair is connected");
    Check(graph.islands.size()==2,"one two-cell island and one isolated child");
    Check(graph.edges[0].first==1 && graph.edges[0].second==2,"canonical edge orientation");
    const auto total=WeatherConvectiveSurrogate::Inventory(state);
    Check(WeatherConvectiveInteractions::Apply(state,graph,60),"valid short-step interactions");
    Check(std::abs(WeatherConvectiveSurrogate::Inventory(state)-total)<1e-10,
        "pair interactions conserve vapour inventory");
    Check(state.children[0].vapour>1.0 && state.children[1].vapour<3.0,
        "vapour equalizes symmetrically");
    Check(state.children[2].vapour==2.0,"uncoupled cell remains unchanged");
    auto reordered=Three();
    std::reverse(reordered.children.begin(),reordered.children.end());
    const auto reorderedGraph=WeatherConvectiveInteractions::Build(reordered);
    Check(reorderedGraph.edges.size()==graph.edges.size() &&
          reorderedGraph.edges[0].first==graph.edges[0].first,
          "input order does not change edge IDs");
    Check(WeatherConvectiveInteractions::Apply(reordered,reorderedGraph,60),
        "reverse order integrates");
    std::sort(reordered.children.begin(),reordered.children.end(),
        [](const auto& a,const auto& b){return a.id<b.id;});
    Check(std::abs(reordered.children[0].vapour-state.children[0].vapour)<1e-12,
        "reordering preserves conserved outcome");
    auto invalid=Three();
    Check(!WeatherConvectiveInteractions::Apply(invalid,graph,1e7),
        "arbitrarily stretched numerical step refused");
    WeatherZoneEvolutionState initial=WeatherConvectiveSurrogate::Initial(77,0);
    WeatherStepTicket shortTicket{77,0,60,WeatherStepKind::Detailed,1};
    StormZoneWeights weights;weights.convection=0.9F;
    auto result=EvaluateInteractingWeather(initial,weights,shortTicket);
    Check(result.has_value(),"interacting evaluator returns completed step");
    if(result)
        Check(std::abs(WeatherConvectiveSurrogate::Inventory(result->state)-
              WeatherConvectiveSurrogate::Inventory(initial))<1e-10,
              "combined evolution and interactions conserve inventory");
    shortTicket.kind=WeatherStepKind::RequiresLongStepModel;
    Check(!EvaluateInteractingWeather(initial,weights,shortTicket).has_value(),
        "interacting operator cannot bypass timewarp guard");
    Check(failures==0,"all interaction tests");
    return failures?1:0;
}
