#include <orbit/infrastructure_catalog/InfrastructureCatalog.hpp>

#include <cassert>
#include <limits>

int main()
{
    using namespace orbit::infrastructure_catalog;
    Catalog catalog;
    Entry road{};
    road.id = "rural-road";
    road.profile.name = "Rural road";
    road.profile.kind = orbit::paths::PathProfileKind::Road;
    road.profile.widthMeters = 7.0;
    road.rules.designSpeedKmh = 90.0;
    assert(catalog.Register(road));
    assert(!catalog.Register(road));
    Entry rail{};
    rail.id = "ballasted-rail";
    rail.profile.name = "Ballasted rail";
    rail.profile.kind = orbit::paths::PathProfileKind::Rail;
    rail.rules.designSpeedKmh = 120.0;
    assert(catalog.Register(rail));
    auto result = catalog.Resolve({"ballasted-rail", 1, ConstructionMode::Supported,
                                   160.0, TransitionMode::CurvatureEased});
    assert(result.has_value());
    assert(result->profile.kind == orbit::paths::PathProfileKind::Rail);
    assert(result->rules.designSpeedKmh == 160.0);
    assert(result->defaultConstruction == ConstructionMode::Supported);
    assert(catalog.Find("ballasted-rail")->rules.designSpeedKmh == 120.0);
    assert(!catalog.Resolve({"ballasted-rail", 2}).has_value());
    assert(!catalog.Resolve({"missing", 1}).has_value());
    assert(!catalog.Resolve({"rural-road", 1, ConstructionMode::Terrain,
                            std::numeric_limits<double>::quiet_NaN()}).has_value());
    return 0;
}
