#include <orbit/path_geometry/CorridorSweep.hpp>

#include <bit>
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
    for (const auto& band : corridor.bands)
        for (const auto& p : band.profile)
        {
            hash = MixCorridor(hash, Bits(p.lateralMeters));
            hash = MixCorridor(hash, Bits(p.elevationMeters));
        }
    for (const auto& key : corridor.sectionKeys)
    {
        hash = MixCorridor(hash, Bits(key.stationMeters));
        for (const auto& band : key.bands)
            for (const auto& p : band.profile)
            {
                hash = MixCorridor(hash, Bits(p.lateralMeters));
                hash = MixCorridor(hash, Bits(p.elevationMeters));
            }
    }
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
