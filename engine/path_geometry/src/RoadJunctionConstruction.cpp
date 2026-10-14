#include <orbit/path_geometry/RoadJunctionConstruction.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace orbit::path_geometry
{
namespace
{
using math::Double3;
bool Finite(Double3 v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
constexpr u32 kBad = std::numeric_limits<u32>::max();
struct Cut
{
    const JunctionApproach* input{};
    u32 stationIndex{0};
    Double3 outward{};
    Double3 up{};
    Double3 centre{};
    std::vector<Double3> boundary;
};
struct RingPoint
{
    Double3 position{};
    scene::ObjectId edge{};
    u32 column{};
    f64 angle{0};
};
Double3 Hermite(Double3 a,Double3 b,Double3 v0,Double3 v1,f64 t)
{
    const f64 t2=t*t,t3=t*t2;
    return a*(2*t3-3*t2+1.0)+v0*(t3-2*t2+t)+
           b*(-2*t3+3*t2)+v1*(t3-t2);
}
std::vector<CollisionTriangle> ToCollision(const PathMesh& mesh)
{
    std::vector<CollisionTriangle> triangles;
    triangles.reserve(mesh.indices.size()/3U);
    for(std::size_t i=0;i+2<mesh.indices.size();i+=3)
    {
        triangles.push_back({
            mesh.vertices[mesh.indices[i]].position,
            mesh.vertices[mesh.indices[i+1]].position,
            mesh.vertices[mesh.indices[i+2]].position
        });
    }
    return triangles;
}
PathMesh TrimBands(const PathMesh& source,
                   std::size_t stationCount,
                   u32 cutIndex, bool atStart)
{
    // Built-in rural-road construction: pavement (3 columns), two shoulder
    // bands (2 columns each). No inference from shuffled triangle indices.
    const u32 columns[]{3U,2U,2U};
    std::vector<u32> mapped(source.vertices.size(),kBad);
    PathMesh mesh;
    mesh.vertices.reserve(source.vertices.size());
    std::size_t base=0;
    for(const u32 width:columns)
    {
        for(std::size_t station=0;station<stationCount;++station)
        {
            const bool keep=atStart ? station>=cutIndex : station<=cutIndex;
            if(!keep)continue;
            for(u32 column=0;column<width;++column)
            {
                const auto old=base+station*width+column;
                mapped[old]=static_cast<u32>(mesh.vertices.size());
                mesh.vertices.push_back(source.vertices[old]);
            }
        }
        base+=stationCount*width;
    }
    mesh.indices.reserve(source.indices.size());
    for(std::size_t i=0;i+2<source.indices.size();i+=3)
    {
        const auto a=source.indices[i],b=source.indices[i+1],
                   c=source.indices[i+2];
        if(a>=mapped.size()||b>=mapped.size()||c>=mapped.size())
            throw std::invalid_argument("Out-of-bounds road mesh triangle.");
        if(mapped[a]==kBad || mapped[b]==kBad || mapped[c]==kBad)continue;
        mesh.indices.insert(mesh.indices.end(),
            {mapped[a],mapped[b],mapped[c]});
    }
    return mesh;
}
} // namespace

RoadJunctionConstruction BuildRoadJunctionConstruction(
    const JunctionPlan& junction,
    const std::span<const JunctionApproach> approaches,
    const RoadJunctionBuildOptions options)
{
    RoadJunctionConstruction result;
    result.node=junction.node;
    result.frame=junction.frame;
    const auto fail=[&](RoadJunctionBuildIssue issue,
                        scene::ObjectId edge,const char* message)
    {
        result.findings.push_back({issue,edge,message});
    };
    if(!junction.node || !junction.frame ||
       !std::isfinite(options.trimDistanceMeters) ||
       options.trimDistanceMeters<=0 ||
       !std::isfinite(options.maximumNormalAngleRadians) ||
       options.maximumNormalAngleRadians<=0 ||
       !std::isfinite(options.maximumJointElevationDifferenceMeters) ||
       options.maximumJointElevationDifferenceMeters<0 ||
       options.connectorSegments<2U || options.connectorSegments>128U ||
       options.maximumApproaches<2U || options.maximumApproaches>16U ||
       options.maximumInputVertices<32U || options.maximumOutputVertices<32U)
    {
        fail(RoadJunctionBuildIssue::InvalidInput,{},"Invalid junction construction options.");
        return result;
    }
    if(!junction.Valid() || junction.hasRailApproach ||
       approaches.size()<2 || approaches.size()>options.maximumApproaches ||
       approaches.size()!=junction.validEdges.size())
    {
        fail(RoadJunctionBuildIssue::InvalidTopology,{},
             "Requires complete all-road junction topology with 2..8 valid approaches.");
        return result;
    }
    std::size_t totalVertices=0;
    std::map<scene::ObjectId,Cut> cuts;
    const auto up=math::Normalize(approaches.front().geometry?
        (approaches.front().atStart?
            approaches.front().geometry->stations.front().up:
            approaches.front().geometry->stations.back().up):
        Double3{});
    if(!Finite(up)||math::LengthSquared(up)<0.9)
    {
        fail(RoadJunctionBuildIssue::InvalidInput,{},"Invalid junction up frame.");
        return result;
    }
    for(const auto& approach:approaches)
    {
        const auto* geom=approach.geometry;
        if(approach.mode!=TransportMode::Road)
        {
            fail(RoadJunctionBuildIssue::UnsupportedMode,approach.edge,
                 "Road patch cannot combine road and railway products.");
            return result;
        }
        if(!geom || geom->stations.size()<3 ||
           geom->edge!=approach.edge)
        {
            fail(RoadJunctionBuildIssue::MissingCorridor,approach.edge,
                 "Road construction requires a complete 3+ station corridor.");
            return result;
        }
        if(geom->frame!=junction.frame || approach.frame!=junction.frame)
        {
            fail(RoadJunctionBuildIssue::MismatchedFrame,approach.edge,
                 "Cross-frame junction construction requires a timed transform.");
            return result;
        }
        const auto n=geom->stations.size();
        if(geom->visualMesh.vertices.size()!=n*7U ||
           geom->visualMesh.indices.size()%3U!=0U ||
           geom->lanes.empty())
        {
            fail(RoadJunctionBuildIssue::UnsupportedSection,approach.edge,
                 "Requires built-in 3+2+2 road profile. Custom profiles need mapping metadata.");
            return result;
        }
        for(const auto& lane:geom->lanes)
            if(lane.points.size()!=n)
            {
                fail(RoadJunctionBuildIssue::UnsupportedSection,approach.edge,
                    "Lane and constructed station samples must match exactly.");
                return result;
            }
        if(totalVertices>options.maximumInputVertices ||
           geom->visualMesh.vertices.size()>
                options.maximumInputVertices-totalVertices)
        {
            fail(RoadJunctionBuildIssue::BudgetExceeded,approach.edge,
                "Combined road source geometry exceeds bounded work budget.");
            return result;
        }
        totalVertices+=geom->visualMesh.vertices.size();
        const auto& endpoint=approach.atStart?
            geom->stations.front():geom->stations.back();
        const auto outward=math::Normalize(
            endpoint.tangent*(approach.atStart?1.0:-1.0));
        const auto normal=math::Normalize(endpoint.up);
        if(!Finite(outward) || math::LengthSquared(outward)<0.9 ||
           !Finite(normal) || math::Dot(normal,up)<
                std::cos(options.maximumNormalAngleRadians) ||
           math::Length(endpoint.position-junction.centre)>0.15)
        {
            fail(RoadJunctionBuildIssue::MismatchedFrame,approach.edge,
                 "Endpoint position or up orientation differs from junction datum.");
            return result;
        }
        u32 cutIndex=0;
        if(approach.atStart)
        {
            while(cutIndex+2<n &&
                  geom->stations[cutIndex].stationMeters-
                    geom->stations.front().stationMeters<
                    options.trimDistanceMeters)
                ++cutIndex;
            if(cutIndex==0U || cutIndex+1>=n)
            {
                fail(RoadJunctionBuildIssue::TooShort,approach.edge,
                     "Approach lacks usable road after trimming.");
                return result;
            }
        }
        else
        {
            cutIndex=static_cast<u32>(n-1);
            while(cutIndex>=2U &&
                  geom->stations.back().stationMeters-
                    geom->stations[cutIndex].stationMeters<
                    options.trimDistanceMeters)
                --cutIndex;
            if(cutIndex==n-1 || cutIndex==0U)
            {
                fail(RoadJunctionBuildIssue::TooShort,approach.edge,
                     "Approach lacks usable road after trimming.");
                return result;
            }
        }
        const auto& cut=geom->stations[cutIndex];
        if(!Finite(cut.position) ||
           std::abs(math::Dot(cut.position-junction.centre,up))>
                options.maximumJointElevationDifferenceMeters)
        {
            fail(RoadJunctionBuildIssue::ExcessiveGrade,approach.edge,
                 "Road patch elevation exceeds allowed cut-plane mismatch.");
            return result;
        }
        Cut record;
        record.input=&approach;
        record.stationIndex=cutIndex;
        record.outward=outward;
        record.up=normal;
        record.centre=cut.position;
        // The construction patch must close BOTH shoulders as well as
        // pavement. Their two shared pavement-edge vertices are included
        // only once, giving five unique lateral section points.
        const std::size_t shoulderL=3U*n+2U*cutIndex;
        const std::size_t shoulderR=5U*n+2U*cutIndex+1U;
        const std::size_t cutVertices[]{
            shoulderL,3U*cutIndex,3U*cutIndex+1U,
            3U*cutIndex+2U,shoulderR
        };
        for(const auto vertex:cutVertices)
        {
            const auto v=geom->visualMesh.vertices[vertex].position;
            if(!Finite(v))
            {
                fail(RoadJunctionBuildIssue::InvalidInput,approach.edge,
                    "Nonfinite road cut boundary.");
                return result;
            }
            record.boundary.push_back(v);
        }
        if(!cuts.emplace(approach.edge,std::move(record)).second)
        {
            fail(RoadJunctionBuildIssue::InvalidTopology,approach.edge,
                "Duplicate road junction edge ID.");
            return result;
        }
    }
    // Read all cut boundaries before constructing; enforce simple, radial,
    // non-overlapping sections. A cut cross-section must occupy five
    // consecutive positions in angular order.
    const auto axisX=math::Normalize(
        cuts.begin()->second.outward-up*
            math::Dot(cuts.begin()->second.outward,up));
    if(!Finite(axisX)||math::LengthSquared(axisX)<0.9)
    {
        fail(RoadJunctionBuildIssue::InvalidInput,{},
             "Road junction tangent projects to zero in local surface plane.");
        return result;
    }
    const auto axisY=math::Normalize(math::Cross(up,axisX));
    std::vector<RingPoint> ring;
    ring.reserve(cuts.size()*5U);
    for(const auto& [id,cut]:cuts)
        for(u32 column=0;column<5U;++column)
        {
            const auto p=cut.boundary[column]-junction.centre;
            const f64 x=math::Dot(p,axisX),y=math::Dot(p,axisY);
            if(x*x+y*y<1.0)
            {
                fail(RoadJunctionBuildIssue::OverlappingApproaches,id,
                     "Cut ring point lies too close to the junction centre.");
                return result;
            }
            ring.push_back({cut.boundary[column],id,column,std::atan2(y,x)});
        }
    std::sort(ring.begin(),ring.end(),[](const RingPoint& a,const RingPoint& b){
        if(a.angle!=b.angle)return a.angle<b.angle;
        if(a.edge!=b.edge)return a.edge<b.edge;
        return a.column<b.column;
    });
    // All boundary vertices belonging to one approach must be contiguous
    // (cyclically) and appear in profile order, otherwise the cuts interleave
    // or form a self-intersecting patch perimeter.
    for(const auto& [id,cut]:cuts)
    {
        u32 switches=0;
        for(std::size_t i=0;i<ring.size();++i)
            if(ring[i].edge==id && ring[(i+ring.size()-1)%ring.size()].edge!=id)
                ++switches;
        if(switches!=1U)
        {
            fail(RoadJunctionBuildIssue::OverlappingApproaches,id,
                 "Approach cuts overlap or angularly interleave.");
            return result;
        }
    }
    // The origin must lie in the radial kernel. More than half a revolution
    // without a boundary produces a fan that extends outside the road patch.
    for(std::size_t i=0;i<ring.size();++i)
    {
        const f64 current=ring[i].angle;
        const f64 next=i+1<ring.size()?ring[i+1].angle:
            ring.front().angle+2.0*3.14159265358979323846;
        if(next-current>=3.14159265358979323846-1.e-5 ||
           next-current<1.e-7)
        {
            fail(RoadJunctionBuildIssue::OverlappingApproaches,{},
                 "Junction cut ring is not star-shaped around the node.");
            return result;
        }
    }
    std::size_t outputVertices=ring.size()+1U;
    for(const auto& [id,cut]:cuts)
    {
        const auto n=cut.input->geometry->stations.size();
        const auto retained= cut.input->atStart?
            n-cut.stationIndex : static_cast<std::size_t>(cut.stationIndex+1U);
        if(retained>options.maximumOutputVertices/7U ||
           retained*7U>options.maximumOutputVertices-outputVertices)
        {
            fail(RoadJunctionBuildIssue::BudgetExceeded,id,
                 "Junction output vertex budget exceeded.");
            return result;
        }
        outputVertices+=retained*7U;
    }
    // Hash only authoritative derived generations and cut geometry. This
    // permits a later atomic mesh/contact cache to reject stale publication.
    u64 signature=0xcbf29ce484222325ULL;
    const auto mix=[&](u64 part){
        signature^=part;
        signature*=0x100000001b3ULL;
    };
    mix(junction.node.high);mix(junction.node.low);
    mix(junction.frame.high);mix(junction.frame.low);
    mix(std::bit_cast<u64>(options.trimDistanceMeters));
    mix(options.connectorSegments);
    for(const auto& [id,cut]:cuts)
    {
        mix(id.high);mix(id.low);
        mix(cut.input->geometry->buildSignature);
        mix(cut.input->geometry->sourceRevision);
        mix(cut.stationIndex);
    }
    result.dependencySignature=signature;

    // Build the patch with a single interior vertex and shared EXACT cut
    // boundary positions. Retained strips are compacted without a second
    // sampling process. Collision triangles are generated from these meshes.
    result.pavement.vertices.push_back({
        .position=junction.centre,.normal=up
    });
    for(const auto& v:ring)
        result.pavement.vertices.push_back({
            .position=v.position,.normal=up});
    for(u32 i=1;i<=ring.size();++i)
        result.pavement.indices.insert(result.pavement.indices.end(),
            {0U,i,i==ring.size()?1U:i+1U});
    result.pavementCollision=ToCollision(result.pavement);
    for(const auto& [id,cut]:cuts)
    {
        const auto& source=cut.input->geometry->visualMesh;
        RoadJunctionTrimmedEdge edge;
        edge.edge=id;
        edge.atStart=cut.input->atStart;
        edge.sourceCutStation=cut.stationIndex;
        edge.cutStationMeters=
            cut.input->geometry->stations[cut.stationIndex].stationMeters;
        try
        {
            edge.retainedMesh=TrimBands(source,
                cut.input->geometry->stations.size(),cut.stationIndex,
                cut.input->atStart);
        }
        catch(const std::invalid_argument&)
        {
            fail(RoadJunctionBuildIssue::UnsupportedSection,id,
                "Invalid source mesh indices during corridor trimming.");
            return result;
        }
        edge.collision=ToCollision(edge.retainedMesh);
        result.edges.push_back(std::move(edge));
    }
    // No routing pointer into the old corridor beyond the cut is published:
    // retarget every road lane movement's endpoints to exact retained
    // lane samples, then use bounded Hermite control directions.
    for(const auto& move:junction.movements)
    {
        if(move.mode!=TransportMode::Road)
            continue;
        const auto a=cuts.find(move.incomingEdge);
        const auto b=cuts.find(move.outgoingEdge);
        if(a==cuts.end()||b==cuts.end())
        {
            fail(RoadJunctionBuildIssue::InvalidTopology,{},
                "Candidate road movement references an absent cut edge.");
            return result;
        }
        const auto& ai=a->second;
        const auto& bi=b->second;
        if(move.incomingLane>=ai.input->geometry->lanes.size() ||
           move.outgoingLane>=bi.input->geometry->lanes.size())
        {
            fail(RoadJunctionBuildIssue::InvalidTopology,{},
                "Candidate road lane index is outside source geometry.");
            return result;
        }
        const auto p0=ai.input->geometry->lanes[move.incomingLane].
            points[ai.stationIndex].position;
        const auto p1=bi.input->geometry->lanes[move.outgoingLane].
            points[bi.stationIndex].position;
        if(!Finite(p0)||!Finite(p1))
        {
            fail(RoadJunctionBuildIssue::InvalidInput,{},
                "Invalid road connector contact points.");
            return result;
        }
        const f64 length=math::Length(p1-p0);
        if(!std::isfinite(length)||length<0.01||length>100.0)
        {
            fail(RoadJunctionBuildIssue::OverlappingApproaches,{},
                "Road connector cut points are degenerate or too distant.");
            return result;
        }
        const f64 handle=std::clamp(length*0.45,0.5,15.0);
        JunctionMovement connector{
            .incomingEdge=move.incomingEdge,
            .outgoingEdge=move.outgoingEdge,
            .incomingLane=move.incomingLane,
            .outgoingLane=move.outgoingLane,
            .mode=TransportMode::Road
        };
        connector.points.reserve(options.connectorSegments+1U);
        for(u32 j=0;j<=options.connectorSegments;++j)
        {
            const f64 t=static_cast<f64>(j)/options.connectorSegments;
            const auto point=Hermite(p0,p1,
                ai.outward*(-handle),bi.outward*handle,t);
            if(!connector.points.empty())
                connector.lengthMeters+=math::Length(
                    point-connector.points.back());
            connector.points.push_back(point);
        }
        result.laneConnectors.push_back(std::move(connector));
    }
    return result;
}
} // namespace orbit::path_geometry
