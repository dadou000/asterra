#include <orbit/path_geometry/RoadJunctionContact.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace orbit::path_geometry
{
namespace
{
bool Finite(const math::Double3 p)
{
    return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);
}
}
RoadContactHit ProbeRoadJunctionContact(
    const RoadJunctionScenePacket& packet, const RoadContactProbe probe)
{
    if(!packet.Valid() || probe.frame!=packet.frame ||
       !Finite(probe.origin) || !Finite(probe.direction) ||
       !(std::isfinite(probe.maximumDistanceMeters) &&
         probe.maximumDistanceMeters>0.0) ||
       probe.maximumTriangles==0U || probe.maximumTriangles>2000000U)
        throw std::invalid_argument("Invalid road contact packet or probe input.");

    const f64 directionLength=math::Length(probe.direction);
    if(!(std::isfinite(directionLength) && directionLength>1.e-10))
        throw std::invalid_argument("Road contact direction must be nonzero.");
    const auto direction=probe.direction/directionLength;
    RoadContactHit closest;
    closest.distanceMeters=probe.maximumDistanceMeters;
    const auto inspect=[&](const auto& triangles,
        const scene::ObjectId owner, const bool isJunction)
    {
        for(const auto& tri:triangles)
        {
            if(closest.testedTriangles>=probe.maximumTriangles)
            {
                closest.budgetExceeded=true;
                return false;
            }
            ++closest.testedTriangles;
            // Double precision, two-sided Moller-Trumbore; the caller owns
            // the frame transform and later physical material interpretation.
            const auto e1=tri.b-tri.a, e2=tri.c-tri.a;
            const auto p=math::Cross(direction,e2);
            const f64 determinant=math::Dot(e1,p);
            if(!std::isfinite(determinant) ||
               std::abs(determinant)<1.e-12)continue;
            const f64 inverse=1.0/determinant;
            const auto from=probe.origin-tri.a;
            const f64 u=math::Dot(from,p)*inverse;
            if(u< -1.e-10 || u>1.0+1.e-10)continue;
            const auto q=math::Cross(from,e1);
            const f64 v=math::Dot(direction,q)*inverse;
            if(v< -1.e-10 || u+v>1.0+1.e-10)continue;
            const f64 t=math::Dot(e2,q)*inverse;
            if(!std::isfinite(t) || t<0.0 ||
               t>closest.distanceMeters || t>probe.maximumDistanceMeters)
                continue;
            const auto n=math::Normalize(math::Cross(e1,e2));
            if(!Finite(n) || math::LengthSquared(n)<0.5)continue;
            closest.hit=true;
            closest.owner=owner;
            closest.junctionPatch=isJunction;
            closest.distanceMeters=t;
            closest.position=probe.origin+direction*t;
            closest.normal=math::Dot(n,direction)>0.0?n*-1.0:n;
        }
        return true;
    };
    for(const auto& edge:packet.edges)
        if(!inspect(edge.product.collision,edge.edge,false))return closest;
    for(const auto& patch:packet.patches)
        if(!inspect(patch.collision,patch.node,true))return closest;
    return closest;
}
} // namespace orbit::path_geometry
