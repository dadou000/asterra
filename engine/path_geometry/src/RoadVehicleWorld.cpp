#include <orbit/path_geometry/RoadVehicleWorld.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace orbit::path_geometry
{
namespace
{
using math::Double3;
bool Finite(const Double3 p) noexcept
{
    return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);
}
f64 HorizontalDistance(Double3 a,Double3 b)
{
    a.y=0;b.y=0;return math::Length(a-b);
}
f64 WrapAngle(f64 x)
{
    return std::atan2(std::sin(x),std::cos(x));
}
const RoadJunctionPublishedEdge* FindEdge(
    const RoadJunctionScenePacket& packet,scene::ObjectId id)
{
    for(const auto& edge:packet.edges)if(edge.edge==id)return &edge;
    return nullptr;
}
bool AppendLane(std::vector<Double3>& out,const LaneReference& lane,
                Double3 join,bool leading)
{
    if(lane.points.size()<2U)return false;
    const auto& first=lane.points.front().position;
    const auto& last=lane.points.back().position;
    const bool joinsFront=math::Length(first-join)<0.1;
    const bool joinsBack=math::Length(last-join)<0.1;
    if(!joinsFront && !joinsBack)return false;
    // Leading side terminates at connector start. Destination starts at
    // connector end. Preserve station order only when it fits direction.
    const bool reverse=leading?joinsFront:joinsBack;
    if(reverse)
        for(auto it=lane.points.rbegin();it!=lane.points.rend();++it)
            out.push_back(it->position);
    else
        for(const auto& point:lane.points)
            out.push_back(point.position);
    return true;
}
RoadVehicleInput GuidedControls(RoadVehicleRuntimeActor& actor)
{
    if(!actor.followRoute || !actor.route.Valid())return actor.controls;
    const auto& line=actor.route.waypoints;
    while(actor.nextWaypoint+1U<line.size() &&
          HorizontalDistance(actor.state.centre,line[actor.nextWaypoint])<2.0)
        ++actor.nextWaypoint;
    const auto& target=line[actor.nextWaypoint];
    const auto delta=target-actor.state.centre;
    const f64 desired=std::atan2(delta.z,delta.x);
    const f64 angle=WrapAngle(desired-actor.state.yawRadians);
    RoadVehicleInput result;
    result.steerRadians=std::clamp(angle*0.75,-0.48,0.48);
    // Near final point: stop rather than drive through missing route.
    const bool arrival=actor.nextWaypoint+1U>=line.size() &&
        HorizontalDistance(actor.state.centre,target)<2.0;
    const f64 targetSpeed=arrival?0.0:actor.targetSpeedMetersPerSecond;
    const f64 error=targetSpeed-actor.state.forwardSpeedMetersPerSecond;
    result.throttle=std::clamp(error*0.22,0.0,1.0);
    result.brake=std::clamp(-error*0.28,0.0,1.0);
    return result;
}
bool FiniteState(const RoadVehicleState& s)
{
    return Finite(s.centre) &&
        std::isfinite(s.yawRadians) && std::isfinite(s.pitchRadians) &&
        std::isfinite(s.rollRadians) &&
        std::isfinite(s.forwardSpeedMetersPerSecond);
}
} // namespace

std::optional<RoadVehicleRoute> CompileRoadVehicleRoute(
    const RoadJunctionScenePacket& packet,
    const scene::ObjectId fromEdge,const scene::ObjectId toEdge,
    const u32 fromLane,const u32 toLane)
{
    if(!packet.Valid() || !fromEdge || !toEdge || fromEdge==toEdge)
        return std::nullopt;
    const auto* from=FindEdge(packet,fromEdge);
    const auto* to=FindEdge(packet,toEdge);
    if(!from || !to || from->product.frame!=packet.frame ||
       to->product.frame!=packet.frame)
        return std::nullopt;
    for(const auto& connector:packet.connections)
    {
        if(connector.mode!=TransportMode::Road ||
           connector.incomingEdge!=fromEdge ||
           connector.outgoingEdge!=toEdge ||
           connector.incomingLane!=fromLane ||
           connector.outgoingLane!=toLane ||
           connector.points.size()<2U ||
           connector.points.size()>256U)
            continue;
        const auto fromIt=std::find_if(
            from->product.lanes.begin(),from->product.lanes.end(),
            [&](const LaneReference& l){return l.laneIndex==fromLane;});
        const auto toIt=std::find_if(
            to->product.lanes.begin(),to->product.lanes.end(),
            [&](const LaneReference& l){return l.laneIndex==toLane;});
        if(fromIt==from->product.lanes.end() ||
           toIt==to->product.lanes.end())continue;
        RoadVehicleRoute route;
        route.frame=packet.frame;
        route.packetSignature=packet.buildSignature;
        route.incomingEdge=fromEdge;
        route.outgoingEdge=toEdge;
        if(!AppendLane(route.waypoints,*fromIt,connector.points.front(),true))
            continue;
        for(std::size_t k=1;k<connector.points.size();++k)
            route.waypoints.push_back(connector.points[k]);
        std::vector<Double3> tail;
        if(!AppendLane(tail,*toIt,connector.points.back(),false))
            continue;
        for(std::size_t k=1;k<tail.size();++k)
            route.waypoints.push_back(tail[k]);
        if(route.waypoints.size()>16384U)continue;
        bool good=true;
        for(const auto& point:route.waypoints)
            if(!Finite(point)){good=false;break;}
        if(good && route.Valid())return route;
    }
    return std::nullopt;
}
bool RoadVehicleRuntimeWorld::Bind(const RoadContactSceneOwner& contact)
{
    const auto* snapshot=contact.Snapshot();
    if(!snapshot)return false;
    if(frame_!=snapshot->frame ||
       worldGeneration_!=snapshot->worldGeneration ||
       signature_!=snapshot->packetSignature)
    {
        actors_.clear();
        tick_=0;
    }
    frame_=snapshot->frame;
    worldGeneration_=snapshot->worldGeneration;
    signature_=snapshot->packetSignature;
    return true;
}
void RoadVehicleRuntimeWorld::Clear() noexcept
{
    actors_.clear();
    frame_={};
    signature_=0;
    worldGeneration_=0;
    tick_=0;
}
bool RoadVehicleRuntimeWorld::Spawn(
    const RoadContactSceneOwner& contact,const scene::ObjectId id,
    const RoadVehicleState& initial,RoadVehicleParameters parameters)
{
    if(!id || actors_.size()>=64U || actors_.contains(id) ||
       !FiniteState(initial))return false;
    const auto* source=contact.Snapshot();
    if(!source || !frame_ ||
       source->frame!=frame_ ||
       source->worldGeneration!=worldGeneration_ ||
       source->packetSignature!=signature_)return false;
    // The shared validated step checks full physical parameter ranges
    // without moving a successfully spawned actor.
    auto check=initial;
    RoadVehicleStepReport diagnostic;
    std::string error;
    if(!AdvanceRoadVehicleStep(contact,frame_,parameters,
            {},1.0/120.0,2048U,check,diagnostic,&error))
        return false;
    actors_.emplace(id,RoadVehicleRuntimeActor{
        .id=id,.state=initial,.parameters=parameters
    });
    return true;
}
bool RoadVehicleRuntimeWorld::Remove(scene::ObjectId id)
{
    return actors_.erase(id)>0U;
}
bool RoadVehicleRuntimeWorld::SetControls(
    scene::ObjectId id,RoadVehicleInput input)
{
    const auto it=actors_.find(id);
    if(it==actors_.end() ||
       !std::isfinite(input.throttle) ||
       !std::isfinite(input.brake) ||
       !std::isfinite(input.steerRadians) ||
       input.throttle<0.0 || input.throttle>1.0 ||
       input.brake<0.0 || input.brake>1.0 ||
       std::abs(input.steerRadians)>0.6)
        return false;
    it->second.controls=input;
    it->second.followRoute=false;
    return true;
}
bool RoadVehicleRuntimeWorld::FollowRoute(
    scene::ObjectId id,RoadVehicleRoute route,const f64 targetSpeed)
{
    const auto it=actors_.find(id);
    if(it==actors_.end() || !route.Valid() ||
       route.frame!=frame_ || route.packetSignature!=signature_ ||
       !std::isfinite(targetSpeed) ||
       targetSpeed<=0 || targetSpeed>30.0)return false;
    for(const auto& waypoint:route.waypoints)
        if(!Finite(waypoint))return false;
    // The spawned actor may already be partway along the incoming road.
    // Start at the closest forward waypoint, never steer toward the far
    // beginning of the lane after joining an active route.
    std::size_t closest=0U;
    f64 best=std::numeric_limits<f64>::infinity();
    for(std::size_t k=0;k<route.waypoints.size();++k)
    {
        const f64 d=HorizontalDistance(
            it->second.state.centre,route.waypoints[k]);
        if(d<best){best=d;closest=k;}
    }
    it->second.route=std::move(route);
    it->second.targetSpeedMetersPerSecond=targetSpeed;
    it->second.nextWaypoint=static_cast<u32>(
        std::min(closest+1U,it->second.route.waypoints.size()-1U));
    it->second.followRoute=true;
    return true;
}
bool RoadVehicleRuntimeWorld::StopFollowing(scene::ObjectId id)
{
    const auto it=actors_.find(id);
    if(it==actors_.end())return false;
    it->second.followRoute=false;
    it->second.controls={};
    return true;
}
const RoadVehicleRuntimeActor* RoadVehicleRuntimeWorld::Find(
    scene::ObjectId id) const noexcept
{
    const auto it=actors_.find(id);
    return it==actors_.end()?nullptr:&it->second;
}
RoadVehicleWorldStep RoadVehicleRuntimeWorld::Step(
    const RoadContactSceneOwner& contact,const u32 count)
{
    RoadVehicleWorldStep result;
    result.packetSignature=signature_;
    result.actorCount=static_cast<u32>(actors_.size());
    result.tick=tick_;
    const auto* source=contact.Snapshot();
    if(!frame_ || !signature_ || !source ||
       source->frame!=frame_ ||
       source->worldGeneration!=worldGeneration_ ||
       source->packetSignature!=signature_)
    {
        result.error="Road vehicle contact generation changed; rebind and respawn.";
        return result;
    }
    if(count==0U || count>120U)
    {
        result.error="Vehicle world step budget must be 1..120 fixed steps.";
        return result;
    }
    constexpr f64 dt=1.0/120.0;
    for(u32 step=0;step<count;++step)
    {
        auto proposed=actors_;
        for(auto& [id,actor]:proposed)
        {
            auto input=GuidedControls(actor);
            std::string error;
            if(!AdvanceRoadVehicleStep(
                contact,frame_,actor.parameters,input,dt,2048U,
                actor.state,actor.lastContact,&error))
            {
                result.error=error.empty()?
                    "Failed vehicle contact step.":error;
                return result;
            }
        }
        // Atomic across all actors at each fixed-step boundary.
        actors_.swap(proposed);
        ++tick_;
        ++result.completedSteps;
        result.tick=tick_;
    }
    result.success=true;
    return result;
}
} // namespace orbit::path_geometry
