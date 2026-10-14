#include <orbit/path_geometry/RailInterlocking.hpp>

namespace orbit::path_geometry
{
namespace
{
bool DefinitionValid(const RailSwitchDefinition& d)
{
    return d.node && d.stem && d.straight && d.diverging &&
        d.stem!=d.straight && d.stem!=d.diverging &&
        d.straight!=d.diverging;
}
bool IsRoute(const RailSwitchDefinition& d,
             scene::ObjectId from,scene::ObjectId to,
             SwitchLeg& leg)
{
    if ((from==d.stem && to==d.straight) ||
        (from==d.straight && to==d.stem))
    {
        leg=SwitchLeg::Straight;return true;
    }
    if ((from==d.stem && to==d.diverging) ||
        (from==d.diverging && to==d.stem))
    {
        leg=SwitchLeg::Diverging;return true;
    }
    return false;
}
}
RailLockResult RailInterlocking::Define(const RailSwitchDefinition d)
{
    if(!DefinitionValid(d))return RailLockResult::InvalidDefinition;
    const auto it=switches_.find(d.node);
    if(it!=switches_.end()&&(it->second.owner || it->second.occupied))
        return RailLockResult::Busy;
    switches_[d.node]=RailSwitchState{.definition=d};
    return RailLockResult::Granted;
}
RailLockResult RailInterlocking::Reserve(
    scene::ObjectId node,scene::ObjectId route,
    scene::ObjectId from,scene::ObjectId to)
{
    const auto it=switches_.find(node);
    if(it==switches_.end())return RailLockResult::MissingSwitch;
    if(!route)return RailLockResult::InvalidRoute;
    auto& state=it->second;
    SwitchLeg leg{};
    if(!IsRoute(state.definition,from,to,leg))
        return RailLockResult::InvalidRoute;
    if(state.owner && state.owner!=route)return RailLockResult::Busy;
    if(state.occupied && !state.owner)return RailLockResult::Occupied;
    if(state.owner==route && state.aligned!=leg)return RailLockResult::Busy;
    state.aligned=leg;
    state.owner=route;
    return RailLockResult::Granted;
}
RailLockResult RailInterlocking::Release(
    scene::ObjectId node,scene::ObjectId route)
{
    const auto it=switches_.find(node);
    if(it==switches_.end())return RailLockResult::MissingSwitch;
    if(!route || it->second.owner!=route)return RailLockResult::WrongOwner;
    if(it->second.occupied)return RailLockResult::Occupied;
    it->second.owner={};
    return RailLockResult::Granted;
}
RailLockResult RailInterlocking::SetOccupied(scene::ObjectId node,bool occupied)
{
    const auto it=switches_.find(node);
    if(it==switches_.end())return RailLockResult::MissingSwitch;
    it->second.occupied=occupied;
    return RailLockResult::Granted;
}
RailLockResult RailInterlocking::SetAlignment(scene::ObjectId node,SwitchLeg leg)
{
    const auto it=switches_.find(node);
    if(it==switches_.end())return RailLockResult::MissingSwitch;
    if(it->second.owner)return RailLockResult::Busy;
    if(it->second.occupied)return RailLockResult::Occupied;
    it->second.aligned=leg;
    return RailLockResult::Granted;
}
bool RailInterlocking::MayTraverse(
    scene::ObjectId node,scene::ObjectId from,scene::ObjectId to,
    scene::ObjectId route) const noexcept
{
    const auto it=switches_.find(node);
    if(it==switches_.end() || !route || it->second.owner!=route)
        return false;
    SwitchLeg needed{};
    return IsRoute(it->second.definition,from,to,needed) &&
        it->second.aligned==needed;
}
const RailSwitchState* RailInterlocking::Find(scene::ObjectId node) const noexcept
{
    const auto it=switches_.find(node);
    return it==switches_.end()?nullptr:&it->second;
}
} // namespace orbit::path_geometry
