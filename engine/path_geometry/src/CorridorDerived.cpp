#include <orbit/path_geometry/CorridorSweep.hpp>

#include <bit>
#include <cmath>
#include <limits>
#include <vector>
#include <utility>
#include <stdexcept>

namespace orbit::path_geometry
{
namespace
{
[[nodiscard]] u64 MixCorridor(u64 hash, const u64 v) noexcept
{
    hash ^= v + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
    return hash;
}
[[nodiscard]] u64 Bits(const f64 value) noexcept
{
    return std::bit_cast<u64>(value);
}

void RebuildRefinedReferences(PathDerivedProduct& product,
                              const CorridorSweep& corridor)
{
    const auto& stations=product.stations;
    product.navigation.clear();
    product.lanes.clear();
    product.referenceGraph={};
    product.debugLines.clear();
    if (stations.empty()) return;
    if (product.laneCount>0 &&
        stations.size()>std::numeric_limits<u32>::max()/product.laneCount)
        throw std::length_error("Refined lane graph index overflow.");

    const f64 halfWidth=product.widthMeters*0.5;
    std::vector<math::Double3> left(stations.size()),right(stations.size());
    std::vector<f64> leftDot(stations.size(),
        std::numeric_limits<f64>::infinity());
    std::vector<f64> rightDot(stations.size(),
        -std::numeric_limits<f64>::infinity());
    std::size_t bandOffset=0;
    for (const auto& band:corridor.bands)
    {
        const std::size_t columns=band.profile.size();
        for (std::size_t i=0;i<stations.size();++i)
        {
            const auto& s=stations[i];
            for (std::size_t j=0;j<columns;++j)
            {
                const auto& p=product.visualMesh.vertices[
                    bandOffset+i*columns+j].position;
                const f64 offset=math::Dot(p-s.position,s.lateral);
                if (offset<leftDot[i]) {leftDot[i]=offset;left[i]=p;}
                if (offset>rightDot[i]) {rightDot[i]=offset;right[i]=p;}
            }
        }
        bandOffset+=stations.size()*columns;
    }

    // Use actual swept surface geometry for lane sample heights and banking.
    // The first band is the construction's primary usable surface (pavement
    // or formation). Other bands (shoulders, railheads) are separate products.
    const std::size_t primaryColumns=corridor.bands.front().profile.size();
    const auto surfaceAt=[&](std::size_t index, f64 offset)
    {
        const auto& s=stations[index];
        const f64 bank=corridor.bankRadians.empty()?0.0:
            corridor.bankRadians[index];
        const auto sideways=s.lateral*std::cos(bank)+s.up*std::sin(bank);
        for (std::size_t j=0;j+1<primaryColumns;++j)
        {
            const auto& a=product.visualMesh.vertices[index*primaryColumns+j].position;
            const auto& b=product.visualMesh.vertices[index*primaryColumns+j+1U].position;
            const f64 x=math::Dot(a-s.position,sideways);
            const f64 y=math::Dot(b-s.position,sideways);
            if (offset>=x && offset<=y && y>x+1.e-12)
                return a+(b-a)*((offset-x)/(y-x));
        }
        return s.position+sideways*offset;
    };
    for (const auto& s:stations)
        product.navigation.push_back({
            .stationMeters=s.stationMeters,
            .position=s.position,
            .tangent=s.tangent,
            .halfWidthMeters=halfWidth,
            .lanes=product.laneCount});
    for (std::size_t i=1;i<stations.size();++i)
    {
        product.debugLines.push_back({stations[i-1].position,
                                      stations[i].position,
                                      DebugLineKind::Centerline});
        product.debugLines.push_back({left[i-1],left[i],
                                      DebugLineKind::Boundary});
        product.debugLines.push_back({right[i-1],right[i],
                                      DebugLineKind::Boundary});
    }
    if (!product.laneCount) return;
    const f64 laneWidth=product.widthMeters/static_cast<f64>(product.laneCount);
    for (u32 lane=0;lane<product.laneCount;++lane)
    {
        const f64 offset=-halfWidth+laneWidth*(static_cast<f64>(lane)+0.5);
        LaneReference reference{.laneIndex=lane,.lateralOffsetMeters=offset};
        reference.points.reserve(stations.size());
        const u32 nodeBase=static_cast<u32>(product.referenceGraph.nodes.size());
        for (std::size_t i=0;i<stations.size();++i)
        {
            const auto& s=stations[i];
            const auto at=surfaceAt(i,offset);
            reference.points.push_back({s.stationMeters,at,s.tangent});
            const u32 index=static_cast<u32>(product.referenceGraph.nodes.size());
            product.referenceGraph.nodes.push_back({
                .index=index,.laneIndex=lane,.stationMeters=s.stationMeters,
                .position=at,.tangent=s.tangent});
            if (i)
            {
                const u32 from=nodeBase+static_cast<u32>(i-1U);
                product.referenceGraph.edges.push_back({
                    .from=from,.to=index,
                    .lengthMeters=math::Length(at-product.referenceGraph.nodes[from].position),
                    .bidirectional=true});
                product.debugLines.push_back({
                    reference.points[i-1U].position,at,DebugLineKind::Lane});
            }
        }
        product.lanes.push_back(std::move(reference));
    }
}
}
PathDerivedProduct BuildCorridorDerived(
    const PathCenterline& centerline,
    const paths::PathProfile& profile,
    const CorridorSweep& corridor,
    const PathBuildOptions options,
    const CorridorRefinementOptions refinement)
{
    PathDerivedProduct product = BuildPathDerived(centerline, profile, options);
    const auto refined = RefineCorridor(product.stations, corridor, refinement);
    const PathMesh mesh = BuildCorridorMesh(refined.stations, refined.corridor);
    if (mesh.vertices.empty() || mesh.indices.empty())
        throw std::invalid_argument("Corridor must generate a non-empty mesh.");

    product.stations = refined.stations;
    product.visualMesh = mesh;
    RebuildRefinedReferences(product,refined.corridor);
    product.collision.clear();
    product.collision.reserve(mesh.indices.size() / 3U);
    for (std::size_t index = 0; index < mesh.indices.size(); index += 3U)
    {
        product.collision.push_back({
            .a = mesh.vertices[mesh.indices[index]].position,
            .b = mesh.vertices[mesh.indices[index + 1U]].position,
            .c = mesh.vertices[mesh.indices[index + 2U]].position
        });
    }

    // Cached identity includes all authored profile geometry and refinement
    // settings, not only the original path centerline.
    u64 hash = MixCorridor(product.buildSignature, 0x434f525249444f52ULL);
    hash = MixCorridor(hash, corridor.bands.size());
    for (const auto& band : corridor.bands)
    {
        hash = MixCorridor(hash, band.profile.size());
        for (const auto& p : band.profile)
        {
            hash = MixCorridor(hash, Bits(p.lateralMeters));
            hash = MixCorridor(hash, Bits(p.elevationMeters));
        }
    }
    hash = MixCorridor(hash, corridor.sectionKeys.size());
    for (const auto& key : corridor.sectionKeys)
    {
        hash = MixCorridor(hash, Bits(key.stationMeters));
        hash = MixCorridor(hash, key.bands.size());
        for (const auto& band : key.bands)
        {
            hash = MixCorridor(hash, band.profile.size());
            for (const auto& p : band.profile)
            {
                hash = MixCorridor(hash, Bits(p.lateralMeters));
                hash = MixCorridor(hash, Bits(p.elevationMeters));
            }
        }
    }
    hash = MixCorridor(hash, corridor.bankRadians.size());
    for (const auto angle : corridor.bankRadians)
        hash = MixCorridor(hash, Bits(angle));
    hash = MixCorridor(hash, Bits(refinement.maximumChordErrorMeters));
    hash = MixCorridor(hash, Bits(refinement.maximumBankStepRadians));
    hash = MixCorridor(hash, Bits(refinement.maximumSegmentLengthMeters));
    hash = MixCorridor(hash, refinement.maximumSubdivisions);
    product.buildSignature = hash;
    return product;
}
} // namespace orbit::path_geometry
