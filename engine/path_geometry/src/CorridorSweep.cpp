#include <orbit/path_geometry/CorridorSweep.hpp>
#include <algorithm>
#include <orbit/world/PlanetTileNeighborhood.hpp>
#include <set>
#include <tuple>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace orbit::path_geometry
{
std::optional<std::vector<terrain::PhysicalTerrainPageAddress>>
ProjectCorridorToTerrainPages(
    const world::PlanetDefinition& planet,
    const std::vector<math::Double3>& bodyCentredPositions,
    const f64 influenceRadiusMeters,
    const u8 pageLevel,
    const std::size_t maximumPages)
{
    if (!planet.id || !std::isfinite(planet.radiusMeters) ||
        planet.radiusMeters <= 0.0 || !std::isfinite(influenceRadiusMeters) ||
        influenceRadiusMeters < 0.0 || bodyCentredPositions.size() < 2 ||
        pageLevel > 24 || maximumPages == 0)
        return std::nullopt;
    const world::PlanetTileId sampleTile{
        .face = world::CubeFace::PositiveX, .level = pageLevel};
    const f64 tileWidth = world::ApproximateTileWidthMeters(planet, sampleTile);
    if (!std::isfinite(tileWidth) || tileWidth <= 0.0)
        return std::nullopt;
    // Along-track sampling prevents missing an intermediate physical page.
    // The neighborhood radius encloses the whole construction footprint.
    const f64 stepMeters = std::max(1.0, tileWidth * 0.25);
    const u32 radiusTiles = static_cast<u32>(
        std::ceil(influenceRadiusMeters / tileWidth)) + 2U;
    if (radiusTiles > 64U) return std::nullopt;
    std::vector<terrain::PhysicalTerrainPageAddress> result;
    std::set<std::tuple<u8,u8,u32,u32>> visited;
    std::size_t samples = 0;
    for (std::size_t i=1; i<bodyCentredPositions.size(); ++i)
    {
        const auto a = bodyCentredPositions[i-1];
        const auto b = bodyCentredPositions[i];
        if (!std::isfinite(a.x) || !std::isfinite(a.y) ||
            !std::isfinite(a.z) || !std::isfinite(b.x) ||
            !std::isfinite(b.y) || !std::isfinite(b.z) ||
            math::LengthSquared(a) < 1.0 || math::LengthSquared(b) < 1.0)
            return std::nullopt;
        const f64 arc = planet.radiusMeters *
            std::acos(std::clamp(math::Dot(math::Normalize(a),
                math::Normalize(b)), -1.0, 1.0));
        const auto steps = static_cast<std::size_t>(
            std::ceil(arc / stepMeters));
        if (steps > 32768U || samples + steps + 1U > 32768U)
            return std::nullopt;
        samples += steps + 1U;
        for (std::size_t j=0; j<=steps; ++j)
        {
            const f64 t = steps ? static_cast<f64>(j)/steps : 0.0;
            const auto direction = math::Normalize(a*(1.0-t) + b*t);
            if (math::LengthSquared(direction) < 0.5) return std::nullopt;
            const auto tile = world::TileForDirection(direction, pageLevel);
            for (const auto& neighbor : world::TileNeighborhood(tile, radiusTiles))
            {
                const auto key = std::make_tuple(
                    static_cast<u8>(neighbor.face), neighbor.level,
                    neighbor.x, neighbor.y);
                if (visited.insert(key).second)
                {
                    if (result.size() >= maximumPages) return std::nullopt;
                    result.push_back({.planet=planet.id, .tile=neighbor});
                }
            }
        }
    }
    return result;
}

CorridorTerrainBlend BlendCorridorTerrainHeight(
    const f64 sourceHeightMeters,
    const CorridorTerrainSample& sample)
{
    if (!std::isfinite(sourceHeightMeters) ||
        !std::isfinite(sample.targetHeightMeters) ||
        !std::isfinite(sample.influence))
        throw std::invalid_argument("Terrain composition inputs must be finite.");
    const f64 influence = std::clamp(sample.influence, 0.0, 1.0);
    const f64 height = sourceHeightMeters +
        (sample.targetHeightMeters - sourceHeightMeters) * influence;
    return {
        .heightMeters = height,
        .cutMeters = std::max(0.0, sourceHeightMeters - height),
        .fillMeters = std::max(0.0, height - sourceHeightMeters),
        .influence = influence
    };
}

std::optional<CorridorTerrainEnvelope> CorridorFootprintEnvelope(
    const std::vector<PathStation>& stations,
    const CorridorTerrainFootprint& footprint)
{
    if (stations.size() < 2 || !std::isfinite(footprint.halfWidthMeters) ||
        !std::isfinite(footprint.shoulderMeters) ||
        !std::isfinite(footprint.blendMeters) ||
        footprint.halfWidthMeters <= 0.0 || footprint.shoulderMeters < 0.0 ||
        footprint.blendMeters < 0.0)
        return std::nullopt;
    const f64 radius = footprint.halfWidthMeters +
        footprint.shoulderMeters + footprint.blendMeters;
    CorridorTerrainEnvelope bounds{
        .minimum = {std::numeric_limits<f64>::infinity(),
                    std::numeric_limits<f64>::infinity(),
                    std::numeric_limits<f64>::infinity()},
        .maximum = {-std::numeric_limits<f64>::infinity(),
                    -std::numeric_limits<f64>::infinity(),
                    -std::numeric_limits<f64>::infinity()}
    };
    for (const auto& station : stations)
    {
        if (!std::isfinite(station.position.x) ||
            !std::isfinite(station.position.y) ||
            !std::isfinite(station.position.z))
            return std::nullopt;
        bounds.minimum.x = std::min(bounds.minimum.x, station.position.x-radius);
        bounds.minimum.y = std::min(bounds.minimum.y, station.position.y-radius);
        bounds.minimum.z = std::min(bounds.minimum.z, station.position.z-radius);
        bounds.maximum.x = std::max(bounds.maximum.x, station.position.x+radius);
        bounds.maximum.y = std::max(bounds.maximum.y, station.position.y+radius);
        bounds.maximum.z = std::max(bounds.maximum.z, station.position.z+radius);
    }
    return bounds;
}

std::optional<CorridorTerrainSample> SampleCorridorTerrain(
    const std::vector<PathStation>& stations,
    const math::Double3& point,
    const CorridorTerrainFootprint& footprint)
{
    if (stations.size() < 2 || !std::isfinite(footprint.halfWidthMeters) ||
        !std::isfinite(footprint.shoulderMeters) ||
        !std::isfinite(footprint.blendMeters) ||
        footprint.halfWidthMeters <= 0.0 || footprint.shoulderMeters < 0.0 ||
        footprint.blendMeters < 0.0)
        return std::nullopt;

    const f64 core = footprint.halfWidthMeters + footprint.shoulderMeters;
    const f64 outer = core + footprint.blendMeters;
    f64 bestDistSquared = std::numeric_limits<f64>::infinity();
    CorridorTerrainSample best{};
    bool found = false;
    // Project in the station local tangent/lateral plane, not global XZ:
    // this also supports arbitrarily oriented local construction frames.
    for (std::size_t i = 0; i + 1 < stations.size(); ++i)
    {
        const auto& a = stations[i];
        const auto& b = stations[i+1];
        const auto delta = b.position - a.position;
        const f64 lengthSquared = math::LengthSquared(delta);
        if (lengthSquared < 1.0e-12) continue;
        const auto up = math::Normalize(a.up + b.up);
        if (math::LengthSquared(up) < 0.5) continue;
        const auto horizontal = delta - up * math::Dot(delta, up);
        const f64 horizontalSquared = math::LengthSquared(horizontal);
        if (horizontalSquared < 1.0e-12) continue;
        const auto horizontalPoint = point - a.position -
            up * math::Dot(point - a.position, up);
        const f64 t = std::clamp(
            math::Dot(horizontalPoint, horizontal) / horizontalSquared, 0.0, 1.0);
        const auto axisPoint = a.position + delta * t;
        const auto localUp = math::Normalize(a.up * (1.0 - t) + b.up * t);
        const auto lateral = math::Normalize(a.lateral * (1.0 - t) + b.lateral * t);
        if (math::LengthSquared(localUp) < 0.5 ||
            math::LengthSquared(lateral) < 0.5) continue;
        const auto offset = point - axisPoint;
        const f64 signedLateral = math::Dot(offset, lateral);
        const auto planarResidual = offset - localUp * math::Dot(offset, localUp);
        const f64 distanceSquared = math::LengthSquared(planarResidual);
        if (distanceSquared >= bestDistSquared) continue;
        bestDistSquared = distanceSquared;
        best.signedLateralMeters = signedLateral;
        best.stationMeters = a.stationMeters +
            (b.stationMeters - a.stationMeters) * t;
        best.targetHeightMeters = math::Dot(axisPoint, localUp);
        const f64 distance = std::sqrt(distanceSquared);
        const f64 blend = footprint.blendMeters;
        const f64 x = blend > 0.0 ?
            std::clamp((outer - distance) / blend, 0.0, 1.0) :
            (distance <= core ? 1.0 : 0.0);
        best.influence = distance <= core ? 1.0 : x*x*(3.0-2.0*x);
        found = true;
    }
    return found && bestDistSquared <= outer*outer ?
        std::optional<CorridorTerrainSample>{best} : std::nullopt;
}

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
    // Derive normals from the *actual* swept triangles, not the station up
    // vector: crowns, banking, elevation keyframes and curved alignments all
    // modify the physical surface.
    std::vector<math::Double3> accumulated(mesh.vertices.size());
    for (std::size_t k = 0; k + 2 < mesh.indices.size(); k += 3)
    {
        const auto ia = mesh.indices[k], ib = mesh.indices[k+1],
                   ic = mesh.indices[k+2];
        const auto& a = mesh.vertices[ia].position;
        const auto& b = mesh.vertices[ib].position;
        const auto& c = mesh.vertices[ic].position;
        auto face = math::Cross(b-a, c-a);
        // Sweep winding is not an up-vector guarantee; choose the
        // hemisphere of the frame's authored upward normal.
        if (math::Dot(face, mesh.vertices[ia].normal) < 0.0)
            face = face * -1.0;
        accumulated[ia] = accumulated[ia] + face;
        accumulated[ib] = accumulated[ib] + face;
        accumulated[ic] = accumulated[ic] + face;
    }
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i)
        if (math::LengthSquared(accumulated[i]) > 1.0e-20)
            mesh.vertices[i].normal = math::Normalize(accumulated[i]);
    return mesh;
}
} // namespace orbit::path_geometry
