#include <orbit/path_geometry/TransportSections.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace orbit::path_geometry
{
namespace
{
using math::Double3;
bool Finite(f64 value) { return std::isfinite(value); }
bool Finite(Double3 v) { return Finite(v.x) && Finite(v.y) && Finite(v.z); }
void CheckRoad(const RoadSectionDesign& d)
{
    if (!d.laneCount || d.laneCount > 32 ||
        !Finite(d.laneWidthMeters) || d.laneWidthMeters <= 0 ||
        !Finite(d.shoulderMeters) || d.shoulderMeters < 0 ||
        !Finite(d.crownSlope) || d.crownSlope < 0 || d.crownSlope > 0.25 ||
        !Finite(d.shoulderSlope) || d.shoulderSlope < 0 || d.shoulderSlope > 0.25)
        throw std::invalid_argument("Invalid road section.");
}
void CheckRail(const RailSectionDesign& d)
{
    if (!d.trackCount || d.trackCount > 32 ||
        !Finite(d.gaugeMeters) || d.gaugeMeters <= 0 ||
        !Finite(d.trackSpacingMeters) ||
        (d.trackCount > 1 && d.trackSpacingMeters <= d.gaugeMeters) ||
        !Finite(d.railheadWidthMeters) || d.railheadWidthMeters <= 0 ||
        d.railheadWidthMeters >= d.gaugeMeters ||
        !Finite(d.railheadHeightMeters) || d.railheadHeightMeters < 0 ||
        !Finite(d.formationMarginMeters) || d.formationMarginMeters <= 0 ||
        !Finite(d.sleeperPitchMeters) || d.sleeperPitchMeters <= 0)
        throw std::invalid_argument("Invalid railway section.");
}
void CheckStations(const std::vector<PathStation>& stations,
                   const std::vector<f64>& cant)
{
    if (!cant.empty() && cant.size() != stations.size())
        throw std::invalid_argument("Rail cant must match station count.");
    for (std::size_t i=0;i<stations.size();++i)
    {
        const auto& s=stations[i];
        if (!Finite(s.stationMeters) || !Finite(s.position) ||
            !Finite(s.tangent) || !Finite(s.up) || !Finite(s.lateral) ||
            math::LengthSquared(s.tangent)<0.5 ||
            math::LengthSquared(s.up)<0.5 ||
            math::LengthSquared(s.lateral)<0.5 ||
            (i && s.stationMeters<=stations[i-1].stationMeters) ||
            (!cant.empty() && !Finite(cant[i])))
            throw std::invalid_argument("Invalid railway station or cant.");
    }
}
f64 TrackOffset(const RailSectionDesign& d, u32 i)
{
    return (static_cast<f64>(i) - (static_cast<f64>(d.trackCount)-1.0)*0.5)
           * d.trackSpacingMeters;
}
struct Axes { Double3 tangent, lateral, up; };
Axes Banked(const Double3& tangentInput, const Double3& upInput,
            const Double3& lateralInput, f64 cant)
{
    const auto tangent=math::Normalize(tangentInput);
    auto up=math::Normalize(upInput-tangent*math::Dot(upInput,tangent));
    if (math::LengthSquared(up)<0.5)
        throw std::invalid_argument("Degenerate rail frame.");
    auto lateral=math::Normalize(math::Cross(tangent,up));
    if (math::Dot(lateral,lateralInput)<0) lateral=lateral*-1.0;
    up=math::Normalize(math::Cross(lateral,tangent));
    const f64 c=std::cos(cant),s=std::sin(cant);
    return {tangent,lateral*c+up*s,up*c-lateral*s};
}
} // namespace

CorridorSweep MakeRoadSweep(const RoadSectionDesign& d,
                            std::vector<f64> bankRadians)
{
    CheckRoad(d);
    for (auto v:bankRadians) if (!Finite(v))
        throw std::invalid_argument("Nonfinite road bank.");
    const f64 half=static_cast<f64>(d.laneCount)*d.laneWidthMeters*0.5;
    const f64 edge=-half*d.crownSlope;
    CorridorSweep out;
    out.bankRadians=std::move(bankRadians);
    out.bands.push_back({{{-half,edge},{0.0,0.0},{half,edge}}});
    if (d.shoulderMeters>0)
    {
        const f64 outer=edge-d.shoulderMeters*d.shoulderSlope;
        out.bands.push_back({{{-half-d.shoulderMeters,outer},{-half,edge}}});
        out.bands.push_back({{{half,edge},{half+d.shoulderMeters,outer}}});
    }
    return out;
}
CorridorSweep MakeRailSweep(const RailSectionDesign& d,
                            std::vector<f64> cantRadians)
{
    CheckRail(d);
    for (auto v:cantRadians) if (!Finite(v))
        throw std::invalid_argument("Nonfinite railway cant.");
    CorridorSweep out;
    out.bankRadians=std::move(cantRadians);
    const f64 half=(static_cast<f64>(d.trackCount)-1.0)*0.5*d.trackSpacingMeters
        +d.gaugeMeters*0.5+d.formationMarginMeters;
    out.bands.push_back({{{-half,-0.10},{0,0},{half,-0.10}}});
    for (u32 t=0;t<d.trackCount;++t)
    {
        const f64 centre=TrackOffset(d,t);
        for (f64 side:{-0.5,0.5})
        {
            const f64 x=centre+side*d.gaugeMeters;
            out.bands.push_back({{{x-d.railheadWidthMeters*0.5,
                                   d.railheadHeightMeters},
                                  {x+d.railheadWidthMeters*0.5,
                                   d.railheadHeightMeters}}});
        }
    }
    return out;
}
std::vector<RailContactReference> RailContactReferences(
    const std::vector<PathStation>& stations, const RailSectionDesign& d,
    const std::vector<f64>& cantRadians)
{
    CheckRail(d);
    CheckStations(stations,cantRadians);
    if (stations.size()>std::numeric_limits<std::size_t>::max()/d.trackCount)
        throw std::length_error("Rail contact reference overflow.");
    std::vector<RailContactReference> result;
    result.reserve(stations.size()*d.trackCount);
    for (std::size_t i=0;i<stations.size();++i)
    {
        const auto& s=stations[i];
        const auto frame=Banked(s.tangent,s.up,s.lateral,
            cantRadians.empty()?0.0:cantRadians[i]);
        for (u32 track=0;track<d.trackCount;++track)
        {
            const auto centre=s.position+frame.lateral*TrackOffset(d,track)
                +frame.up*d.railheadHeightMeters;
            result.push_back({s.stationMeters,track,
                centre-frame.lateral*(d.gaugeMeters*0.5),
                centre+frame.lateral*(d.gaugeMeters*0.5),
                frame.tangent,frame.up});
        }
    }
    return result;
}
std::vector<RailSleeperPlacement> RailSleeperPlacements(
    const std::vector<PathStation>& stations, const RailSectionDesign& d,
    const std::vector<f64>& cantRadians, std::size_t maximumInstances)
{
    CheckRail(d);
    CheckStations(stations,cantRadians);
    if (stations.size()<2) return {};
    const f64 distance=stations.back().stationMeters-stations.front().stationMeters;
    const f64 count=std::floor(distance/d.sleeperPitchMeters)+1.0;
    if (!Finite(count) || count<1 || count*static_cast<f64>(d.trackCount)>
        static_cast<f64>(maximumInstances))
        throw std::length_error("Sleeper instance budget exceeded.");
    std::vector<RailSleeperPlacement> result;
    result.reserve(static_cast<std::size_t>(count)*d.trackCount);
    std::size_t segment=0;
    for (std::size_t n=0;n<static_cast<std::size_t>(count);++n)
    {
        const f64 at=stations.front().stationMeters+static_cast<f64>(n)*d.sleeperPitchMeters;
        if (at>stations.back().stationMeters) break;
        while (segment+2<stations.size() && stations[segment+1].stationMeters<at)
            ++segment;
        const auto& a=stations[segment],&b=stations[segment+1];
        const f64 alpha=std::clamp((at-a.stationMeters)/
            (b.stationMeters-a.stationMeters),0.0,1.0);
        const f64 cant=cantRadians.empty()?0.0:
            cantRadians[segment]*(1.0-alpha)+cantRadians[segment+1]*alpha;
        const auto frame=Banked(
            a.tangent*(1.0-alpha)+b.tangent*alpha,
            a.up*(1.0-alpha)+b.up*alpha,
            a.lateral*(1.0-alpha)+b.lateral*alpha,cant);
        const auto centre=a.position*(1.0-alpha)+b.position*alpha;
        for (u32 track=0;track<d.trackCount;++track)
            result.push_back({at,track,
                centre+frame.lateral*TrackOffset(d,track),
                frame.tangent,frame.lateral,frame.up});
    }
    return result;
}
} // namespace orbit::path_geometry
