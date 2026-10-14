#include <orbit/path_geometry/TransportJunctions.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace orbit::path_geometry
{
namespace
{
using math::Double3;
bool Finite(Double3 p) { return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z); }
struct Port
{
    JunctionApproach approach;
    Double3 centre{}, towards{}, outward{}, up{}, lateral{};
    f64 halfWidth{0.0};
};
Double3 SampleHermite(Double3 a, Double3 b, Double3 va, Double3 vb, f64 t)
{
    const f64 t2=t*t, t3=t2*t;
    return a*(2*t3-3*t2+1) + va*(t3-2*t2+t) +
           b*(-2*t3+3*t2) + vb*(t3-t2);
}
const LaneReference* LaneAt(const Port& port,u32 index)
{
    const auto& lanes=port.approach.geometry->lanes;
    return index<lanes.size()?&lanes[index]:nullptr;
}
Double3 InteriorPoint(const Port& port,u32 index)
{
    const auto& product=*port.approach.geometry;
    const std::size_t n=product.stations.size();
    // For the supported one-track rail recipe the primary lane reference
    // lies on ballast formation, not the wheel contact plane. Derive the
    // track centre from the actual rendered paired railheads instead.
    // Formation: 3*n vertices; left/right contact strips: 2*n each.
    if(port.approach.mode==TransportMode::Rail && index==0 &&
       n>=2 && product.visualMesh.vertices.size()==n*7U)
    {
        const std::size_t i=port.approach.atStart?1U:n-2U;
        const auto& v=product.visualMesh.vertices;
        return (v[3U*n+2U*i].position+
                v[3U*n+2U*i+1U].position+
                v[5U*n+2U*i].position+
                v[5U*n+2U*i+1U].position)*0.25;
    }
    const auto* lane=LaneAt(port,index);
    const auto& points=lane->points;
    return port.approach.atStart?points[1].position:points[points.size()-2].position;
}
std::vector<Double3> Hull(std::vector<Double3> samples, Double3 origin,
                          Double3 axisX, Double3 axisY)
{
    struct V {Double3 p; f64 x{},y{};};
    std::vector<V> points;
    for (const auto& p:samples)
    {
        const auto d=p-origin;
        points.push_back({p,math::Dot(d,axisX),math::Dot(d,axisY)});
    }
    std::sort(points.begin(),points.end(),[](const V& a,const V& b){
        return a.x!=b.x?a.x<b.x:a.y<b.y;
    });
    points.erase(std::unique(points.begin(),points.end(),[](const V& a,const V& b){
        return std::abs(a.x-b.x)<1.e-8 && std::abs(a.y-b.y)<1.e-8;
    }),points.end());
    if(points.size()<3) return {};
    const auto cross=[](const V& a,const V& b,const V& c){
        return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
    };
    std::vector<V> hull;
    for(const auto& p:points)
    {
        while(hull.size()>=2 && cross(hull[hull.size()-2],hull.back(),p)<=1.e-10)
            hull.pop_back();
        hull.push_back(p);
    }
    const auto lower=hull.size();
    for(std::size_t i=points.size()-1;i>0;--i)
    {
        const auto& p=points[i-1];
        while(hull.size()>lower && cross(hull[hull.size()-2],hull.back(),p)<=1.e-10)
            hull.pop_back();
        hull.push_back(p);
    }
    if(!hull.empty()) hull.pop_back();
    std::vector<Double3> result;
    for(const auto& p:hull)result.push_back(p.p);
    return result;
}
}
JunctionPlan BuildTransportJunction(
    const scene::ObjectId node, const std::span<const JunctionApproach> approaches,
    const JunctionOptions options)
{
    JunctionPlan out;
    out.node=node;
    const auto problem=[&](JunctionIssue issue,scene::ObjectId edge,const char* why){
        out.findings.push_back({issue,edge,why});
    };
    if(!node || !std::isfinite(options.endpointToleranceMeters) ||
       options.endpointToleranceMeters<0 ||
       !std::isfinite(options.minimumApproachMeters) || options.minimumApproachMeters<=0 ||
       !std::isfinite(options.maximumConnectorMeters) || options.maximumConnectorMeters<=0 ||
       options.connectorSegments<2 || options.connectorSegments>256 ||
       options.maximumApproaches<2 || options.maximumMovements==0)
    {
        problem(JunctionIssue::InvalidInput,{},"Invalid junction ID or budget.");
        return out;
    }
    if(approaches.size()>options.maximumApproaches)
    {
        problem(JunctionIssue::TooManyApproaches,{},"Junction exceeds approach budget.");
        return out;
    }
    std::vector<Port> ports;
    ports.reserve(approaches.size());
    std::set<scene::ObjectId> visitedEdges;
    for(const auto& approach:approaches)
    {
        if(approach.edge && !visitedEdges.insert(approach.edge).second)
        {
            problem(JunctionIssue::InvalidInput,approach.edge,
                "Duplicate incident edge at a junction.");
            continue;
        }
        if(!approach.edge || !approach.frame || !approach.geometry ||
           approach.geometry->edge!=approach.edge ||
           approach.geometry->frame!=approach.frame ||
           approach.geometry->stations.size()<2 ||
           approach.geometry->lanes.empty())
        {
            problem(JunctionIssue::MissingGeometry,approach.edge,
                "Missing or mismatched derived corridor geometry.");
            continue;
        }
        const auto& stations=approach.geometry->stations;
        const auto& station=approach.atStart?stations.front():stations.back();
        if(!Finite(station.position)||!Finite(station.tangent)||
           !Finite(station.up)||!Finite(station.lateral))
        {
            problem(JunctionIssue::InvalidInput,approach.edge,
                "Nonfinite station frame.");
            continue;
        }
        const auto toward=math::Normalize(station.tangent*
            (approach.atStart?-1.0:1.0));
        const auto outward=toward*-1.0;
        const f64 half=approach.geometry->navigation.empty()?
            approach.geometry->widthMeters*0.5:
            (approach.atStart?approach.geometry->navigation.front().halfWidthMeters:
                              approach.geometry->navigation.back().halfWidthMeters);
        if(!std::isfinite(half)||half<=0)
        {
            problem(JunctionIssue::InvalidInput,approach.edge,"Invalid approach width.");
            continue;
        }
        bool validLanes=true;
        for(const auto& lane:approach.geometry->lanes)
            if(lane.points.size()<2) validLanes=false;
        if(!validLanes)
        {
            problem(JunctionIssue::GeometryTooShort,approach.edge,
                "Every approach lane needs at least two station samples.");
            continue;
        }
        if(!out.frame){out.frame=approach.frame;out.centre=station.position;}
        if(approach.frame!=out.frame)
        {
            problem(JunctionIssue::MixedFrames,approach.edge,
                "Convert endpoints to one explicit reference frame before connecting.");
            continue;
        }
        if(math::Length(station.position-out.centre)>options.endpointToleranceMeters)
        {
            problem(JunctionIssue::MismatchedEndpoints,approach.edge,
                "Connected semantic node has non-coincident derived endpoints.");
            continue;
        }
        ports.push_back({approach,station.position,toward,outward,station.up,
                         station.lateral,half});
        out.validEdges.push_back(approach.edge);
    }
    std::sort(ports.begin(),ports.end(),[](const Port& a,const Port& b){
        return a.approach.edge<b.approach.edge;
    });
    std::sort(out.validEdges.begin(),out.validEdges.end());
    for(const auto& in:ports)
    {
        // One-way start endpoints cannot be entered, and one-way end
        // endpoints cannot be exited.
        if(in.approach.atStart && !in.approach.bidirectional) continue;
        for(const auto& dest:ports)
        {
            if(in.approach.edge==dest.approach.edge ||
               in.approach.mode!=dest.approach.mode ||
               (!dest.approach.atStart && !dest.approach.bidirectional))
                continue;
            // A switch has one alignment per track; multi-track rail junction
            // moves need an authored turnout map, not all-to-all lane mixing.
            if(in.approach.mode==TransportMode::Rail &&
               (in.approach.geometry->lanes.size()!=1 ||
                dest.approach.geometry->lanes.size()!=1 ||
                in.approach.geometry->visualMesh.vertices.size()!=
                    in.approach.geometry->stations.size()*7U ||
                dest.approach.geometry->visualMesh.vertices.size()!=
                    dest.approach.geometry->stations.size()*7U))
            {
                problem(JunctionIssue::UnsupportedRailTracks,in.approach.edge,
                    "Multi-track rail switch requires explicit track correspondence.");
                continue;
            }
            const u32 count=static_cast<u32>(std::min(
                in.approach.geometry->lanes.size(),
                dest.approach.geometry->lanes.size()));
            for(u32 lane=0;lane<count;++lane)
            {
                if(out.movements.size()>=options.maximumMovements)
                {
                    problem(JunctionIssue::TooManyMovements,in.approach.edge,
                        "Junction movement budget exceeded.");
                    return out;
                }
                const auto a=InteriorPoint(in,lane);
                const auto b=InteriorPoint(dest,lane);
                const f64 distance=math::Length(b-a);
                if(!Finite(a)||!Finite(b)||!std::isfinite(distance)||
                   distance>options.maximumConnectorMeters)
                {
                    problem(JunctionIssue::GeometryTooShort,in.approach.edge,
                        "Invalid or oversized junction movement.");
                    continue;
                }
                const f64 handle=std::max(options.minimumApproachMeters,
                    std::min(12.0, distance*0.45));
                JunctionMovement m{
                    .incomingEdge=in.approach.edge,
                    .outgoingEdge=dest.approach.edge,
                    .incomingLane=lane,.outgoingLane=lane,
                    .mode=in.approach.mode
                };
                const auto va=in.towards*handle;
                const auto vb=dest.outward*handle;
                m.points.reserve(options.connectorSegments+1U);
                for(u32 i=0;i<=options.connectorSegments;++i)
                {
                    const f64 t=static_cast<f64>(i)/options.connectorSegments;
                    const auto point=SampleHermite(a,b,va,vb,t);
                    if(!m.points.empty())m.lengthMeters+=
                        math::Length(point-m.points.back());
                    m.points.push_back(point);
                }
                out.movements.push_back(std::move(m));
            }
        }
    }
    // Low-cost footprint preview is never a collidable, automatically
    // published patch; it only supplies a deterministic hull to future
    // junction mesh construction and editor diagnostics.
    std::vector<Double3> corners;
    for(const auto& p:ports)
    {
        if(p.approach.mode!=TransportMode::Road)continue;
        const auto& s=p.approach.atStart?
            p.approach.geometry->stations.front():
            p.approach.geometry->stations.back();
        corners.push_back(s.position + s.lateral*p.halfWidth);
        corners.push_back(s.position - s.lateral*p.halfWidth);
        const auto extension=p.outward*std::min(4.0,p.halfWidth);
        corners.push_back(s.position+extension+s.lateral*p.halfWidth);
        corners.push_back(s.position+extension-s.lateral*p.halfWidth);
    }
    if(corners.size()>=6 && !ports.empty())
    {
        const Double3 up=math::Normalize(ports.front().up);
        const Double3 ax=math::Normalize(math::Cross(up,ports.front().outward));
        const Double3 ay=math::Normalize(math::Cross(up,ax));
        if(math::LengthSquared(ax)>0.5 && math::LengthSquared(ay)>0.5)
        {
            const auto hull=Hull(std::move(corners),out.centre,ax,ay);
            if(hull.size()>=3 && hull.size()<std::numeric_limits<u32>::max()-1U)
            {
                out.roadFootprint.vertices.push_back({
                    .position=out.centre,.normal=up});
                for(const auto& p:hull)
                    out.roadFootprint.vertices.push_back({
                        .position=p,.normal=up});
                for(u32 i=1;i<=hull.size();++i)
                    out.roadFootprint.indices.insert(
                        out.roadFootprint.indices.end(),{
                            0U,i,i==hull.size()?1U:i+1U});
            }
        }
    }
    return out;
}
} // namespace orbit::path_geometry
