#include <orbit/path_geometry/RailInterlocking.hpp>
#include <iostream>
#define CHECK(x) do {if (!(x)) {std::cerr<<"FAIL " #x " line "<<__LINE__<<'\n';return 1;}} while(false)
int main()
{
    using namespace orbit::path_geometry;
    const orbit::scene::ObjectId node{1,1},stem{2,1},main{2,2},branch{2,3};
    const orbit::scene::ObjectId routeA{3,1},routeB{3,2};
    RailInterlocking control;
    CHECK(control.Define({node,stem,main,branch})==RailLockResult::Granted);
    CHECK(control.Define({node,stem,main,main})==RailLockResult::InvalidDefinition);
    CHECK(control.Reserve(node,routeA,stem,main)==RailLockResult::Granted);
    CHECK(control.MayTraverse(node,stem,main,routeA));
    CHECK(!control.MayTraverse(node,stem,branch,routeA));
    CHECK(!control.MayTraverse(node,stem,main,routeB));
    CHECK(control.Reserve(node,routeB,stem,branch)==RailLockResult::Busy);
    CHECK(control.SetAlignment(node,SwitchLeg::Diverging)==RailLockResult::Busy);
    CHECK(control.SetOccupied(node,true)==RailLockResult::Granted);
    CHECK(control.Release(node,routeA)==RailLockResult::Occupied);
    CHECK(control.SetOccupied(node,false)==RailLockResult::Granted);
    CHECK(control.Release(node,routeB)==RailLockResult::WrongOwner);
    CHECK(control.Release(node,routeA)==RailLockResult::Granted);
    CHECK(control.Reserve(node,routeB,stem,branch)==RailLockResult::Granted);
    CHECK(control.MayTraverse(node,stem,branch,routeB));
    CHECK(control.Reserve(node,routeB,stem,main)==RailLockResult::Busy);
    CHECK(control.Release(node,routeB)==RailLockResult::Granted);
    CHECK(control.Reserve(node,routeA,main,branch)==RailLockResult::InvalidRoute);
    CHECK(control.SetOccupied(node,true)==RailLockResult::Granted);
    CHECK(control.Reserve(node,routeA,stem,main)==RailLockResult::Occupied);
    CHECK(control.SetOccupied(node,false)==RailLockResult::Granted);
    CHECK(control.Define({node,stem,branch,main})==RailLockResult::Granted);
    CHECK(control.Find(node)!=nullptr);
    control.Clear();
    CHECK(control.Find(node)==nullptr);
    CHECK(control.Reserve(node,routeA,stem,main)==RailLockResult::MissingSwitch);
    return 0;
}
