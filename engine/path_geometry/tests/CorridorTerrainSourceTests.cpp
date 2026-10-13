#include <orbit/path_geometry/CorridorTerrainSource.hpp>
#include <cmath>
#include <cstdlib>

namespace
{
class FlatSource final : public orbit::terrain::TerrainSource
{
public:
    [[nodiscard]] orbit::terrain::TerrainSample Sample(
        const orbit::terrain::TerrainQuery&) const noexcept override
    {
        orbit::terrain::TerrainSample s{};
        s.elevationMeters = 20.0;
        s.coarseElevationMeters = 20.0;
        s.climate.temperatureC = 9.0F;
        return s;
    }
    [[nodiscard]] orbit::u64 Revision() const noexcept override { return 7; }
};
void Check(bool valid) { if (!valid) std::abort(); }
}
int main()
{
    FlatSource base;
    orbit::world::PlanetDefinition planet{};
    planet.id = orbit::world::PlanetId::Random();
    planet.radiusMeters = 1000;
    orbit::path_geometry::CorridorTerrainLayer road{};
    road.edge = orbit::scene::ObjectId::Random();
    road.footprint = {4, 1, 2};
    road.stations = {
        {.stationMeters=0, .position={1000,0,-20}, .up={1,0,0}, .lateral={0,1,0}},
        {.stationMeters=40, .position={1000,0,20}, .up={1,0,0}, .lateral={0,1,0}}
    };
    orbit::path_geometry::CorridorTerrainSource overlay(base, planet, {road}, 12);
    orbit::terrain::TerrainQuery query{};
    query.planet = planet.id;
    query.unitDirection = {1,0,0};
    const auto before = base.Sample(query);
    const auto after = overlay.Sample(query);
    Check(before.elevationMeters == 20.0);
    Check(std::abs(after.elevationMeters) < 1.e-8);
    Check(after.climate.temperatureC == before.climate.temperatureC);
    Check(overlay.GenerationRevisions().authoring !=
          base.GenerationRevisions().authoring);
    query.planet = orbit::world::PlanetId::Random();
    Check(overlay.Sample(query).elevationMeters == 20.0);
    return 0;
}
