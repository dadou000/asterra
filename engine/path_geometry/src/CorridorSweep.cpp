#include <orbit/path_geometry/CorridorSweep.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace orbit::path_geometry
{
PathMesh BuildCorridorMesh(
    const std::vector<PathStation>& stations,
    const CorridorSweep& corridor)
{
    PathMesh mesh;
    if (stations.size() < 2 || corridor.bands.empty())
        return mesh;
    if (!corridor.bankRadians.empty() &&
        corridor.bankRadians.size() != stations.size())
        throw std::invalid_argument("Banking requires one angle per station.");
    const auto validateBands = [](const std::vector<CorridorBand>& bands) {
        for (const auto& band : bands)
        {
            if (band.profile.size() < 2)
                throw std::invalid_argument("Corridor band needs at least two profile points.");
            for (std::size_t j = 0; j < band.profile.size(); ++j)
            {
                const auto& pt = band.profile[j];
                if (!std::isfinite(pt.lateralMeters) || !std::isfinite(pt.elevationMeters) ||
                    (j && pt.lateralMeters <= band.profile[j - 1].lateralMeters))
                    throw std::invalid_argument("Profile lateral stations must be strictly increasing and finite.");
            }
        }
    };
    validateBands(corridor.bands);
    for (std::size_t k = 0; k < corridor.sectionKeys.size(); ++k)
    {
        const auto& key = corridor.sectionKeys[k];
        if (!std::isfinite(key.stationMeters) ||
            (k && key.stationMeters <= corridor.sectionKeys[k-1].stationMeters) ||
            key.bands.size() != corridor.bands.size())
            throw std::invalid_argument("Invalid corridor section key topology or station.");
        validateBands(key.bands);
        for (std::size_t j=0;j<key.bands.size();++j)
            if (key.bands[j].profile.size()!=corridor.bands[j].profile.size())
                throw std::invalid_argument("Section key band topology differs.");
    }
    for (std::size_t bandIndex=0; bandIndex<corridor.bands.size(); ++bandIndex)
    {
        const auto& band = corridor.bands[bandIndex];
        if (band.profile.size() < 2)
            throw std::invalid_argument("Corridor band needs at least two profile points.");
        for (std::size_t j = 0; j < band.profile.size(); ++j)
        {
            const auto& pt = band.profile[j];
            if (!std::isfinite(pt.lateralMeters) || !std::isfinite(pt.elevationMeters) ||
                (j && pt.lateralMeters <= band.profile[j - 1].lateralMeters))
                throw std::invalid_argument("Profile lateral stations must be strictly increasing and finite.");
        }
        const std::size_t cols = band.profile.size();
        const std::size_t first = mesh.vertices.size();
        if (stations.size() > (std::numeric_limits<u32>::max() - first) / cols)
            throw std::overflow_error("Corridor vertex index overflow.");
        for (std::size_t i = 0; i < stations.size(); ++i)
        {
            const auto& station = stations[i];
            const f64 bank = corridor.bankRadians.empty() ? 0.0 : corridor.bankRadians[i];
            if (!std::isfinite(bank))
                throw std::invalid_argument("Invalid bank angle.");
            const f64 co = std::cos(bank), si = std::sin(bank);
            // Positive bank rotates lateral toward up, preserving an orthonormal section.
            const auto sideways = station.lateral * co + station.up * si;
            const auto upwards = station.up * co - station.lateral * si;
            // Keyed sections smoothly change width, crown, rail offsets and elevation.
            const CorridorSectionKey* ka=nullptr;
            const CorridorSectionKey* kb=nullptr;
            if (!corridor.sectionKeys.empty())
            {
                auto it=std::upper_bound(corridor.sectionKeys.begin(), corridor.sectionKeys.end(),
                    station.stationMeters, [](f64 value, const CorridorSectionKey& key) {
                        return value < key.stationMeters;
                    });
                if (it==corridor.sectionKeys.begin()) ka=kb=&*it;
                else if (it==corridor.sectionKeys.end()) ka=kb=&corridor.sectionKeys.back();
                else {kb=&*it; ka=&*(it-1);}
            }
            const f64 mix=(ka && kb && ka!=kb)
                ? (station.stationMeters-ka->stationMeters)/(kb->stationMeters-ka->stationMeters) : 0.0;
            for (std::size_t vertex=0; vertex<band.profile.size(); ++vertex)
            {
                CorridorProfilePoint p=band.profile[vertex];
                if (ka)
                {
                    const auto& from=ka->bands[bandIndex].profile[vertex];
                    const auto& to=kb->bands[bandIndex].profile[vertex];
                    p.lateralMeters=from.lateralMeters+(to.lateralMeters-from.lateralMeters)*mix;
                    p.elevationMeters=from.elevationMeters+(to.elevationMeters-from.elevationMeters)*mix;
                }
                mesh.vertices.push_back({
                    .position = station.position + sideways * p.lateralMeters +
                        upwards * p.elevationMeters,
                    .normal = upwards,
                    .uv = {station.stationMeters, p.lateralMeters}
                });
            }
        }
        for (std::size_t i = 0; i + 1 < stations.size(); ++i)
        {
            for (std::size_t j = 0; j + 1 < cols; ++j)
            {
                const u32 a = static_cast<u32>(first + i * cols + j);
                const u32 b = a + 1U;
                const u32 c = a + static_cast<u32>(cols);
                const u32 d = c + 1U;
                mesh.indices.insert(mesh.indices.end(), {a, b, c, b, d, c});
            }
        }
    }
    return mesh;
}
} // namespace orbit::path_geometry
