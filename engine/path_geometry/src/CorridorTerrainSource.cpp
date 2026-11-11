#include <orbit/path_geometry/CorridorTerrainSource.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace orbit::path_geometry
{
CorridorTerrainSource::CorridorTerrainSource(
    const terrain::TerrainSource& source,
    const world::PlanetDefinition planet,
    std::vector<CorridorTerrainLayer> layers,
    const u64 authoringRevision)
    : source_(&source), planet_(planet),
      layers_(std::move(layers)), authoringRevision_(authoringRevision)
{
    if (!planet_.id || !std::isfinite(planet_.radiusMeters) ||
        planet_.radiusMeters <= 0.0)
        throw std::invalid_argument("Corridor terrain source requires a valid planet.");
    std::sort(layers_.begin(), layers_.end(),
        [](const auto& a, const auto& b) { return a.edge < b.edge; });
    for (std::size_t i = 0; i < layers_.size(); ++i)
    {
        if (!layers_[i].edge || layers_[i].stations.size() < 2 ||
            (i && layers_[i-1].edge == layers_[i].edge) ||
            !CorridorFootprintEnvelope(layers_[i].stations,
                                      layers_[i].footprint))
            throw std::invalid_argument("Invalid or duplicate corridor construction layer.");
    }
}

namespace
{
const terrain::TerrainSource& RequireTerrainSource(
    const std::shared_ptr<const terrain::TerrainSource>& source)
{
    if (!source) throw std::invalid_argument("Null owned terrain source.");
    return *source;
}
}

CorridorTerrainSource::CorridorTerrainSource(
    std::shared_ptr<const terrain::TerrainSource> source,
    const world::PlanetDefinition planet,
    std::vector<CorridorTerrainLayer> layers,
    const u64 revision)
    : CorridorTerrainSource(RequireTerrainSource(source), planet, std::move(layers), revision)
{
    ownedSource_ = std::move(source);
    source_ = ownedSource_.get();
}

std::shared_ptr<const CorridorTerrainSource>
CorridorTerrainSource::Rebind(
    std::shared_ptr<const terrain::TerrainSource> source) const
{
    if (!source) throw std::invalid_argument("Cannot rebind null terrain source.");
    return std::make_shared<CorridorTerrainSource>(
        std::move(source), planet_, layers_, authoringRevision_);
}

terrain::TerrainSample CorridorTerrainSource::Sample(
    const terrain::TerrainQuery& query) const noexcept
{
    auto sample = source_->Sample(query);
    if (query.planet != planet_.id ||
        !std::isfinite(sample.elevationMeters) ||
        !std::isfinite(query.unitDirection.x) ||
        !std::isfinite(query.unitDirection.y) ||
        !std::isfinite(query.unitDirection.z))
        return sample;

    const auto direction = math::Normalize(query.unitDirection);
    if (math::LengthSquared(direction) < 0.5) return sample;
    // Query surface location, not camera or global inertial coordinates.
    const auto point = direction *
        (planet_.radiusMeters + sample.elevationMeters);
    for (const auto& layer : layers_)
    {
        const auto target = SampleCorridorTerrain(
            layer.stations, point, layer.footprint);
        if (!target || target->influence <= 0.0) continue;
        // The generic corridor sampler reports a projection onto the
        // interpolated up vector. That projection is NOT the radial distance
        // on a curved planet (and can vary with latitude). Recover the
        // physical axis point at the sampled station and use its radius.
        auto local = *target;
        const auto positionAtStation = [&]() noexcept {
            for (std::size_t i = 0; i + 1 < layer.stations.size(); ++i)
            {
                const auto& a = layer.stations[i];
                const auto& b = layer.stations[i + 1];
                if (local.stationMeters < a.stationMeters ||
                    local.stationMeters > b.stationMeters) continue;
                const f64 length = b.stationMeters - a.stationMeters;
                const f64 t = length > 0.0 ?
                    std::clamp((local.stationMeters - a.stationMeters) / length,
                               0.0, 1.0) : 0.0;
                return a.position + (b.position - a.position) * t;
            }
            return layer.stations.back().position;
        };
        local.targetHeightMeters =
            math::Length(positionAtStation()) - planet_.radiusMeters;
        const auto blend = BlendCorridorTerrainHeight(
            sample.elevationMeters, local);
        sample.elevationMeters = blend.heightMeters;
    }
    return sample;
}

u64 CorridorTerrainSource::Revision() const noexcept
{
    return terrain::StableCombine64(source_->Revision(), authoringRevision_);
}

terrain::TerrainGenerationRevisions
CorridorTerrainSource::GenerationRevisions() const noexcept
{
    auto revisions = source_->GenerationRevisions();
    revisions.authoring = terrain::StableCombine64(
        revisions.authoring, authoringRevision_);
    return revisions;
}
} // namespace orbit::path_geometry
