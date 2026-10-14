#include <orbit/path_geometry/JunctionConflicts.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace orbit::path_geometry
{
namespace
{
using math::Double3;
f64 SegmentDistanceSquared(Double3 a,Double3 b,Double3 c,Double3 d)
{
    // Ericson's closest-approach solution, including degenerate segments.
    // Uses double precision and avoids any global planar projection.
    constexpr f64 eps=1.e-14;
    const auto u=b-a, v=d-c, w=a-c;
    const f64 aa=math::Dot(u,u),bb=math::Dot(u,v);
    const f64 cc=math::Dot(v,v),dd=math::Dot(u,w),ee=math::Dot(v,w);
    f64 s=0.0,t=0.0;
    if(aa<=eps && cc<=eps)return math::LengthSquared(w);
    if(aa<=eps)t=std::clamp(ee/cc,0.0,1.0);
    else if(cc<=eps)s=std::clamp(-dd/aa,0.0,1.0);
    else
    {
        const f64 denominator=aa*cc-bb*bb;
        if(denominator>eps)
            s=std::clamp((bb*ee-cc*dd)/denominator,0.0,1.0);
        t=(bb*s+ee)/cc;
        if(t<0.0)
        {
            t=0.0;
            s=std::clamp(-dd/aa,0.0,1.0);
        }
        else if(t>1.0)
        {
            t=1.0;
            s=std::clamp((bb-dd)/aa,0.0,1.0);
        }
    }
    return math::LengthSquared(w+u*s-v*t);
}
bool Finite(math::Double3 p)
{
    return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);
}
f64 EnvelopeRadius(const JunctionMovement& movement,
                   const JunctionConflictOptions& options)
{
    return movement.mode==TransportMode::Rail?
        options.railSweepRadiusMeters:options.roadSweepRadiusMeters;
}
}
JunctionConflictReport AnalyzeJunctionConflicts(
    const JunctionPlan& plan, const JunctionConflictOptions options)
{
    JunctionConflictReport out;
    if(!plan.node || !std::isfinite(options.roadSweepRadiusMeters) ||
       !std::isfinite(options.railSweepRadiusMeters) ||
       options.roadSweepRadiusMeters<0 || options.railSweepRadiusMeters<0 ||
       options.maximumEvaluatedPairs==0 ||
       options.maximumSegmentComparisons==0 || options.maximumConflicts==0)
        return out;
    for(const auto& m:plan.movements)
    {
        if(m.points.size()<2)return out;
        for(const auto& p:m.points)if(!Finite(p))return out;
    }
    out.valid=true;
    for(std::size_t i=0;i<plan.movements.size();++i)
    {
        const auto& a=plan.movements[i];
        for(std::size_t j=i+1;j<plan.movements.size();++j)
        {
            if(out.evaluatedPairs>=options.maximumEvaluatedPairs ||
               out.conflicts.size()>=options.maximumConflicts)
            {
                out.truncated=true;
                return out;
            }
            ++out.evaluatedPairs;
            const auto& b=plan.movements[j];
            const bool shared=
                (a.incomingEdge==b.incomingEdge &&
                 a.incomingLane==b.incomingLane) ||
                (a.outgoingEdge==b.outgoingEdge &&
                 a.outgoingLane==b.outgoingLane);
            if(shared)
            {
                out.conflicts.push_back({
                    static_cast<u32>(i),static_cast<u32>(j),
                    JunctionConflictKind::SharedLane,0.0});
                continue;
            }
            const f64 radius=EnvelopeRadius(a,options)+EnvelopeRadius(b,options);
            const f64 threshold2=radius*radius;
            f64 closest=std::numeric_limits<f64>::infinity();
            bool overlap=false;
            for(std::size_t ia=1;ia<a.points.size()&&!overlap;++ia)
                for(std::size_t ib=1;ib<b.points.size();++ib)
                {
                    if(out.evaluatedSegmentComparisons>=
                       options.maximumSegmentComparisons)
                    {
                        out.truncated=true;
                        return out;
                    }
                    ++out.evaluatedSegmentComparisons;
                    const f64 d2=SegmentDistanceSquared(
                        a.points[ia-1],a.points[ia],
                        b.points[ib-1],b.points[ib]);
                    closest=std::min(closest,d2);
                    if(d2<=threshold2){overlap=true;break;}
                }
            if(overlap)
                out.conflicts.push_back({
                    static_cast<u32>(i),static_cast<u32>(j),
                    JunctionConflictKind::GeometricOverlap,
                    std::sqrt(closest)});
        }
    }
    return out;
}
} // namespace orbit::path_geometry
