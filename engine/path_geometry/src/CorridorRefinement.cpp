#include <orbit/path_geometry/CorridorSweep.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace orbit::path_geometry
{
namespace
{
using math::Double3;
Double3 At(const PathStation& a, const PathStation& b, const f64 t)
{
    const f64 length = b.stationMeters - a.stationMeters;
    const f64 t2 = t*t, t3 = t2*t;
    return a.position * (2*t3 - 3*t2 + 1) +
           a.tangent * (length * (t3 - 2*t2 + t)) +
           b.position * (-2*t3 + 3*t2) +
           b.tangent * (length * (t3-t2));
}
Double3 Derivative(const PathStation& a, const PathStation& b, const f64 t)
{
    const f64 length = b.stationMeters - a.stationMeters;
    const f64 t2 = t*t;
    return a.position * (6*t2-6*t) +
           a.tangent * (length * (3*t2-4*t+1)) +
           b.position * (-6*t2+6*t) +
           b.tangent * (length * (3*t2-2*t));
}
PathStation Sample(const PathStation& a, const PathStation& b, const f64 t)
{
    PathStation p{};
    p.stationMeters = a.stationMeters + (b.stationMeters-a.stationMeters)*t;
    p.position = At(a,b,t);
    p.tangent = math::Normalize(Derivative(a,b,t));
    auto up = math::Normalize(a.up * (1-t) + b.up*t);
    auto lateral = math::Normalize(math::Cross(p.tangent,up));
    if (math::LengthSquared(lateral) < 1.e-12)
        lateral = math::Normalize(a.lateral * (1-t) + b.lateral*t);
    p.lateral = lateral;
    p.up = math::Normalize(math::Cross(lateral,p.tangent));
    return p;
}
}
RefinedCorridor RefineCorridor(
    const std::vector<PathStation>& source,
    const CorridorSweep& corridor,
    const CorridorRefinementOptions options)
{
    if (!std::isfinite(options.maximumChordErrorMeters) ||
        options.maximumChordErrorMeters <= 0 ||
        !std::isfinite(options.maximumBankStepRadians) ||
        options.maximumBankStepRadians <= 0 ||
        !std::isfinite(options.maximumSegmentLengthMeters) ||
        options.maximumSegmentLengthMeters <= 0 ||
        options.maximumSubdivisions > 20)
        throw std::invalid_argument("Invalid corridor refinement budget.");
    if (!corridor.bankRadians.empty() &&
        corridor.bankRadians.size() != source.size())
        throw std::invalid_argument("Banking count differs from source stations.");
    RefinedCorridor out{{},corridor};
    out.corridor.bankRadians.clear();
    if (source.empty()) return out;
    for (std::size_t i=0;i<source.size();++i)
    {
        const auto& a=source[i];
        if (!std::isfinite(a.stationMeters) ||
            (i && a.stationMeters <= source[i-1].stationMeters))
            throw std::invalid_argument("Source stations must increase.");
        if (i==0) {out.stations.push_back(a);if(!corridor.bankRadians.empty())out.corridor.bankRadians.push_back(corridor.bankRadians[0]);}
        if (i+1==source.size()) break;
        const auto& b=source[i+1];
        if (!(b.stationMeters > a.stationMeters)) throw std::invalid_argument("Source stations must increase.");
        const f64 bankA=corridor.bankRadians.empty()?0:corridor.bankRadians[i];
        const f64 bankB=corridor.bankRadians.empty()?0:corridor.bankRadians[i+1];
        if (!std::isfinite(bankA) || !std::isfinite(bankB))
            throw std::invalid_argument("Invalid source bank.");
        const auto append=[&](auto&& self, f64 lo, f64 hi, u32 depth)->void
        {
            const f64 mid=(lo+hi)*0.5;
            const auto left=At(a,b,lo),right=At(a,b,hi),center=At(a,b,mid);
            const f64 error=math::Length(center-(left+right)*0.5);
            const f64 distance=math::Length(right-left);
            if (depth < options.maximumSubdivisions &&
                (error > options.maximumChordErrorMeters ||
                 distance > options.maximumSegmentLengthMeters ||
                 std::abs(bankB-bankA)*(hi-lo) > options.maximumBankStepRadians))
            {
                self(self,lo,mid,depth+1);
                self(self,mid,hi,depth+1);
            }
            else
            {
                out.stations.push_back(hi==1.0?b:Sample(a,b,hi));
                if (!corridor.bankRadians.empty())
                    out.corridor.bankRadians.push_back(bankA+(bankB-bankA)*hi);
            }
        };
        append(append,0,1,0);
    }
    return out;
}
} // namespace orbit::path_geometry
