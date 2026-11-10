#include <orbit/path_geometry/RoadContactScene.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace orbit::path_geometry
{
namespace
{
using math::Double3;
bool Finite(const Double3 value) noexcept
{
    return std::isfinite(value.x) &&
        std::isfinite(value.y) &&
        std::isfinite(value.z);
}
void Expand(RoadContactBounds& bounds,const Double3 p) noexcept
{
    bounds.minimum.x=std::min(bounds.minimum.x,p.x);
    bounds.minimum.y=std::min(bounds.minimum.y,p.y);
    bounds.minimum.z=std::min(bounds.minimum.z,p.z);
    bounds.maximum.x=std::max(bounds.maximum.x,p.x);
    bounds.maximum.y=std::max(bounds.maximum.y,p.y);
    bounds.maximum.z=std::max(bounds.maximum.z,p.z);
}
bool ValidateMeshContact(const PathMesh& mesh,
                         const std::vector<CollisionTriangle>& collision)
{
    if(mesh.indices.size()%3U!=0U ||
       mesh.indices.size()/3U!=collision.size())return false;
    for(std::size_t i=0;i<collision.size();++i)
    {
        const auto a=mesh.indices[3U*i],b=mesh.indices[3U*i+1U],
                   c=mesh.indices[3U*i+2U];
        if(a>=mesh.vertices.size() || b>=mesh.vertices.size() ||
           c>=mesh.vertices.size())return false;
        const auto& tri=collision[i];
        if(!Finite(tri.a)||!Finite(tri.b)||!Finite(tri.c)||
           tri.a!=mesh.vertices[a].position ||
           tri.b!=mesh.vertices[b].position ||
           tri.c!=mesh.vertices[c].position)return false;
    }
    return true;
}
// Slab broadphase, including parallel-ray axes and the max-distance limit.
bool Intersects(const RoadContactBounds& bounds,const Double3 origin,
                const Double3 direction,const f64 range) noexcept
{
    f64 near=0.0,far=range;
    for(int axis=0;axis<3;++axis)
    {
        const f64 o=axis==0?origin.x:axis==1?origin.y:origin.z;
        const f64 d=axis==0?direction.x:axis==1?direction.y:direction.z;
        const f64 minimum=axis==0?bounds.minimum.x:
            axis==1?bounds.minimum.y:bounds.minimum.z;
        const f64 maximum=axis==0?bounds.maximum.x:
            axis==1?bounds.maximum.y:bounds.maximum.z;
        if(std::abs(d)<1.e-14)
        {
            if(o<minimum || o>maximum)return false;
            continue;
        }
        f64 low=(minimum-o)/d,high=(maximum-o)/d;
        if(low>high)std::swap(low,high);
        near=std::max(near,low);
        far=std::min(far,high);
        if(near>far)return false;
    }
    return true;
}
}
RoadContactSceneBuild PrepareRoadContactScene(
    const RoadJunctionScenePacket& packet,
    const std::size_t maximumShapes,
    const std::size_t maximumTriangles)
{
    RoadContactSceneBuild built;
    const auto fail=[&](const char* message)
    {
        built.error=message;
        built.generation={};
    };
    if(!packet.Valid() || maximumShapes==0U ||
       maximumShapes>4096U || maximumTriangles==0U ||
       maximumTriangles>2000000U ||
       packet.edges.size()>maximumShapes ||
       packet.patches.size()>maximumShapes-packet.edges.size())
    {
        fail("Invalid packet or collision-shape/triangle budget.");
        return built;
    }
    built.generation.frame=packet.frame;
    built.generation.worldGeneration=packet.worldGeneration;
    built.generation.packetSignature=packet.buildSignature;
    std::set<scene::ObjectId> ids;
    const auto append=[&](const scene::ObjectId owner,
                          const bool junctionPatch,
                          const frames::FrameId frame,
                          const PathMesh& mesh,
                          const std::vector<CollisionTriangle>& triangles)
    {
        if(!owner || frame!=packet.frame ||
           !ids.insert(owner).second ||
           triangles.empty() ||
           triangles.size()>maximumTriangles-built.generation.triangleCount ||
           !ValidateMeshContact(mesh,triangles))
        {
            fail("Collision owner duplicated, stale, empty, malformed or beyond budget.");
            return false;
        }
        const f64 hi=std::numeric_limits<f64>::max();
        RoadContactShape shape;
        shape.owner=owner;
        shape.junctionPatch=junctionPatch;
        shape.bounds.minimum={hi,hi,hi};
        shape.bounds.maximum={-hi,-hi,-hi};
        shape.triangles=triangles;
        for(const auto& tri:shape.triangles)
        {
            Expand(shape.bounds,tri.a);
            Expand(shape.bounds,tri.b);
            Expand(shape.bounds,tri.c);
        }
        built.generation.triangleCount+=triangles.size();
        built.generation.shapes.push_back(std::move(shape));
        return true;
    };
    // Deterministic order follows the source publication's stable edge/node
    // ordering. No full untrimmed road is accidentally reintroduced here.
    for(const auto& edge:packet.edges)
        if(!append(edge.edge,false,edge.product.frame,
                   edge.product.visualMesh,edge.product.collision))
            return built;
    for(const auto& patch:packet.patches)
        if(!append(patch.node,true,patch.frame,
                   patch.mesh,patch.collision))
            return built;
    return built;
}

bool RoadContactSceneOwner::Commit(RoadContactSceneBuild candidate)
{
    if(!candidate.Ready())
    {
        lastError_=candidate.error.empty()?
            "Contact generation is not complete.":std::move(candidate.error);
        return false;
    }
    active_=std::move(candidate.generation);
    lastError_.clear();
    return true;
}
void RoadContactSceneOwner::Clear() noexcept
{
    active_={};
    lastError_.clear();
}
const RoadContactSceneGeneration*
RoadContactSceneOwner::Snapshot() const noexcept
{
    return active_.Valid()?&active_:nullptr;
}
RoadContactHit RoadContactSceneOwner::Probe(
    const RoadContactProbe probe) const
{
    const auto* scene=Snapshot();
    if(!scene || scene->frame!=probe.frame ||
       !Finite(probe.origin)||!Finite(probe.direction)||
       !std::isfinite(probe.maximumDistanceMeters) ||
       probe.maximumDistanceMeters<=0 ||
       probe.maximumTriangles==0U || probe.maximumTriangles>2000000U)
        throw std::invalid_argument("No current contact generation or invalid probe.");
    const f64 magnitude=math::Length(probe.direction);
    if(!(std::isfinite(magnitude)&&magnitude>1.e-10))
        throw std::invalid_argument("Contact direction must be nonzero.");
    const Double3 direction=probe.direction/magnitude;
    RoadContactHit hit;
    hit.distanceMeters=probe.maximumDistanceMeters;
    for(const auto& shape:scene->shapes)
    {
        if(!Intersects(shape.bounds,probe.origin,direction,hit.distanceMeters))
            continue;
        for(const auto& triangle:shape.triangles)
        {
            if(hit.testedTriangles>=probe.maximumTriangles)
            {
                hit.budgetExceeded=true;
                return hit;
            }
            ++hit.testedTriangles;
            const auto e1=triangle.b-triangle.a;
            const auto e2=triangle.c-triangle.a;
            const auto cross=math::Cross(direction,e2);
            const f64 det=math::Dot(e1,cross);
            if(!std::isfinite(det)||std::abs(det)<1.e-12)continue;
            const auto w=probe.origin-triangle.a;
            const f64 inv=1.0/det;
            const f64 u=math::Dot(w,cross)*inv;
            if(u< -1.e-10 || u>1.0+1.e-10)continue;
            const auto q=math::Cross(w,e1);
            const f64 v=math::Dot(direction,q)*inv;
            if(v< -1.e-10 || u+v>1.0+1.e-10)continue;
            const f64 distance=math::Dot(e2,q)*inv;
            if(!std::isfinite(distance) || distance<0 ||
               distance>hit.distanceMeters)continue;
            const auto normal=math::Normalize(math::Cross(e1,e2));
            if(!Finite(normal)||math::LengthSquared(normal)<0.5)continue;
            hit.hit=true;
            hit.owner=shape.owner;
            hit.junctionPatch=shape.junctionPatch;
            hit.distanceMeters=distance;
            hit.position=probe.origin+direction*distance;
            hit.normal=math::Dot(normal,direction)>0?
                normal*-1.0:normal;
        }
    }
    return hit;
}
} // namespace orbit::path_geometry
