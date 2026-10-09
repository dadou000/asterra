#include <orbit/studio_session/StudioTerrainSampleRpc.hpp>

#include <orbit/scene/ObjectStore.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain/TectonicStructure.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace orbit::studio_session
{
namespace
{
constexpr std::size_t kMaximumPoints = 4096U;
constexpr f64 kDegrees = 180.0 / std::numbers::pi;

[[nodiscard]] i64 ProcessId() noexcept
{
#if defined(_WIN32)
    return static_cast<i64>(_getpid());
#else
    return static_cast<i64>(getpid());
#endif
}

[[nodiscard]] const rpc::Value::Object& RequireObject(const rpc::Value& params)
{
    if (!params.IsObject()) throw rpc::Error(-32602, "Params must be an object.");
    return params.AsObject();
}

[[nodiscard]] f64 RequireNumber(const rpc::Value::Object& object, const std::string_view key)
{
    const auto found = object.find(key);
    if (found == object.end() || !found->second.IsNumber())
        throw rpc::Error(-32602, std::string(key) + " must be a number.");
    return found->second.AsNumber();
}

[[nodiscard]] f64 OptionalNumber(
    const rpc::Value::Object& values, const std::string_view key, const f64 fallback)
{
    const auto found = values.find(key);
    if (found == values.end()) return fallback;
    if (!found->second.IsNumber()) throw rpc::Error(-32602, std::string(key) + " must be a number.");
    return found->second.AsNumber();
}

[[nodiscard]] bool OptionalBool(
    const rpc::Value::Object& values, const std::string_view key, const bool fallback)
{
    const auto found = values.find(key);
    if (found == values.end()) return fallback;
    if (!found->second.IsBool()) throw rpc::Error(-32602, std::string(key) + " must be a boolean.");
    return found->second.AsBool();
}

[[nodiscard]] math::Double3 DirectionFromLatLon(const f64 latitude, const f64 longitude)
{
    if (!(std::abs(latitude) <= 90.0))
        throw rpc::Error(-32602, "latitude_degrees must be within -90..90.");
    if (!std::isfinite(longitude))
        throw rpc::Error(-32602, "longitude_degrees must be finite.");
    // The flat map convention: +Y north, longitude = atan2(+Z, +X).
    const f64 a = latitude / kDegrees;
    const f64 b = longitude / kDegrees;
    return {std::cos(a) * std::cos(b), std::sin(a), std::cos(a) * std::sin(b)};
}

[[nodiscard]] math::Double3 PointDirection(const rpc::Value& point)
{
    const auto& values = RequireObject(point);
    return DirectionFromLatLon(
        RequireNumber(values, "latitude_degrees"), RequireNumber(values, "longitude_degrees"));
}

struct BoundTerrain
{
    scene::ObjectId object{};
    world::PlanetDefinition planet{};
    const terrain::TerrainSource* source{nullptr};
    const terrain::AnalyticTerrainSource* analytic{nullptr};
};

[[nodiscard]] BoundTerrain ResolveTerrain(StudioSession& session, const rpc::Value::Object& values)
{
    const auto found = values.find("terrain");
    if (found == values.end() || !found->second.IsString())
        throw rpc::Error(-32602, "terrain must be a terrain object id (see terrain.list).");
    const auto object = scene::ObjectId::Parse(found->second.AsString());
    if (!object.has_value()) throw rpc::Error(-32602, "terrain must be a terrain object id.");
    auto& surfaces = session.World().Surfaces();
    const auto body = surfaces.BodyForTerrainObject(*object);
    if (!body.has_value()) throw rpc::Error(1004, "terrain is not bound to a body.");
    const auto planet = surfaces.Registry().SphericalPlanetDefinition(*body);
    if (!planet.has_value()) throw rpc::Error(1004, "terrain body has no spherical planet.");
    const auto* capability = surfaces.Registry().FindTerrainSurface(*body);
    if (capability == nullptr || capability->terrain == nullptr)
        throw rpc::Error(1004, "terrain body has no composed terrain source yet.");
    BoundTerrain bound;
    bound.object = *object;
    bound.planet = *planet;
    bound.source = capability->terrain.get();
    bound.analytic = dynamic_cast<const terrain::AnalyticTerrainSource*>(bound.source);
    return bound;
}

[[nodiscard]] rpc::Value TectonicsBlock(const terrain::TectonicStructureSample& s)
{
    return rpc::Value(rpc::Value::Object{
        {"plate_id", static_cast<i64>(s.plateId)},
        {"neighbour_plate_id", static_cast<i64>(s.neighbourPlateId)},
        {"plate_type", std::string(s.continental ? "continental" : "oceanic")},
        {"neighbour_plate_type", std::string(s.neighbourContinental ? "continental" : "oceanic")},
        {"boundary_type", std::string(terrain::TectonicBoundaryTypeName(s.boundaryType))},
        {"boundary_strength", s.boundaryStrength},
        {"convergence", s.convergence},
        {"divergence", s.divergence},
        {"transform", s.transform},
        {"continental_crust_fraction", s.continentalCrustFraction},
        {"crust_thickness_km", s.crustThicknessKm},
        {"crust_age", s.crustAge},
        {"geological_age", s.geologicalAge},
        {"uplift_meters", s.upliftMeters},
        {"subsidence_meters", s.subsidenceMeters},
        {"stress", s.stress},
        {"volcanism", s.volcanism},
        {"subduction_trench", s.subductionTrench},
        {"volcanic_arc", s.volcanicArc},
        {"structural_elevation_meters", s.structuralElevationMeters},
        {"fracture_density", s.fractureDensity}});
}

[[nodiscard]] rpc::Value SamplePoint(
    const BoundTerrain& bound,
    const math::Double3& direction,
    const f64 footprintMeters,
    const bool tectonics,
    const f64 seaLevelMeters)
{
    const math::Double3 unit = math::Normalize(direction);
    const terrain::TerrainSample sample = bound.source->Sample({
        .unitDirection = unit,
        .footprintMeters = footprintMeters,
        .planet = bound.planet.id,
        .radialOffsetMeters = 0.0});
    const auto& b = sample.biomes;
    rpc::Value::Object out{
        {"latitude_degrees", std::asin(std::clamp(unit.y, -1.0, 1.0)) * kDegrees},
        {"longitude_degrees", std::atan2(unit.z, unit.x) * kDegrees},
        {"elevation_meters", sample.elevationMeters},
        {"coarse_elevation_meters", sample.coarseElevationMeters},
        {"detail_delta_meters", sample.elevationMeters - sample.coarseElevationMeters},
        {"height_above_sea_level_meters", sample.elevationMeters - seaLevelMeters},
        {"standing_water_depth_meters", sample.standingWaterDepthMeters},
        {"underwater", sample.elevationMeters < seaLevelMeters},
        {"temperature_c", static_cast<f64>(sample.climate.temperatureC)},
        {"humidity", static_cast<f64>(sample.climate.humidity)},
        {"precipitation", static_cast<f64>(sample.climate.precipitation)},
        {"continentality", static_cast<f64>(sample.climate.continentality)},
        {"biome_weights", rpc::Value(rpc::Value::Object{
            {"ocean", static_cast<f64>(b.ocean)},
            {"desert", static_cast<f64>(b.desert)},
            {"grassland", static_cast<f64>(b.grassland)},
            {"temperate_forest", static_cast<f64>(b.temperateForest)},
            {"boreal_forest", static_cast<f64>(b.borealForest)},
            {"tundra", static_cast<f64>(b.tundra)},
            {"alpine", static_cast<f64>(b.alpine)},
            {"wetland", static_cast<f64>(b.wetland)}})}};
    if (tectonics && bound.analytic != nullptr)
        out.emplace(
            "tectonics",
            TectonicsBlock(bound.analytic->GlobalFields().SampleTectonicStructure(unit)));
    return rpc::Value(std::move(out));
}
} // namespace

void RegisterStudioTerrainSampleRpc(rpc::Dispatcher& dispatcher, StudioSession& session)
{
    const auto started = std::chrono::steady_clock::now();

    dispatcher.Register(
        {
            .name = "terrain.list",
            .description =
                "Lists the terrain objects of the open world: terrain object id (the 'terrain' "
                "argument of every terrain.* method), name, parent body object, planet radius "
                "and sea level. Read-only.",
            .mutating = false
        },
        [&session](const rpc::Value&)
        {
            rpc::Value::Array terrains;
            auto& surfaces = session.World().Surfaces();
            for (const auto body : surfaces.TerrainBodies())
            {
                const auto object = surfaces.TerrainObjectForBody(body);
                if (!object.has_value()) continue;
                const auto planet = surfaces.Registry().SphericalPlanetDefinition(body);
                const auto* capability = surfaces.Registry().FindTerrainSurface(body);
                const auto* analytic = capability != nullptr
                    ? dynamic_cast<const terrain::AnalyticTerrainSource*>(capability->terrain.get())
                    : nullptr;
                const auto record = session.World().Objects().Find(*object);
                rpc::Value::Object entry{
                    {"terrain", object->ToString()},
                    {"name", record.has_value() ? record->name : std::string()},
                    {"composed", capability != nullptr && capability->terrain != nullptr}};
                if (record.has_value() && record->parent.has_value())
                    entry.emplace("parent", record->parent->ToString());
                if (planet.has_value()) entry.emplace("planet_radius_meters", planet->radiusMeters);
                if (analytic != nullptr)
                    entry.emplace("sea_level_meters", analytic->Description().global.seaLevelMeters);
                terrains.emplace_back(std::move(entry));
            }
            return rpc::Value(rpc::Value::Object{{"terrains", std::move(terrains)}});
        });

    dispatcher.Register(
        {
            .name = "terrain.sample",
            .description =
                "Samples the composed terrain without moving any camera. 'terrain' is a terrain "
                "object id (terrain.list). Give 'points' (up to 4096 objects with "
                "latitude_degrees/longitude_degrees), or a great-circle 'transect' "
                "({from:{latitude_degrees,longitude_degrees}, to:{...}, count 2..4096}). "
                "Each sample returns elevation_meters (the ground/bed the camera collides with), "
                "coarse_elevation_meters, detail_delta_meters, height_above_sea_level_meters, "
                "standing_water_depth_meters, underwater, climate and biome_weights; "
                "include_tectonics=true adds the plate structure (crust, boundary masks, "
                "structural elevation, trench/arc, fracture density). footprint_meters (default "
                "500) is the sample footprint. Read-only; CPU path, so it matches the ground "
                "navigation uses.",
            .mutating = false
        },
        [&session](const rpc::Value& params)
        {
            const auto& values = RequireObject(params);
            const BoundTerrain bound = ResolveTerrain(session, values);
            const f64 footprint =
                std::clamp(OptionalNumber(values, "footprint_meters", 500.0), 1.0, 1.0e6);
            const bool tectonics = OptionalBool(values, "include_tectonics", false);
            const f64 seaLevel = bound.analytic != nullptr
                ? bound.analytic->Description().global.seaLevelMeters
                : 0.0;

            std::vector<math::Double3> directions;
            const auto points = values.find("points");
            const auto transect = values.find("transect");
            if ((points != values.end()) == (transect != values.end()))
                throw rpc::Error(-32602, "Give exactly one of points or transect.");
            if (points != values.end())
            {
                if (!points->second.IsArray()) throw rpc::Error(-32602, "points must be an array.");
                const auto& array = points->second.AsArray();
                if (array.empty() || array.size() > kMaximumPoints)
                    throw rpc::Error(-32602, "points must hold 1..4096 entries.");
                for (const auto& point : array) directions.push_back(PointDirection(point));
            }
            else
            {
                const auto& t = RequireObject(transect->second);
                const auto from = t.find("from");
                const auto to = t.find("to");
                if (from == t.end() || to == t.end())
                    throw rpc::Error(-32602, "transect needs from and to.");
                const f64 count = RequireNumber(t, "count");
                if (!(count >= 2.0 && count <= static_cast<f64>(kMaximumPoints)))
                    throw rpc::Error(-32602, "transect count must be 2..4096.");
                const math::Double3 a = PointDirection(from->second);
                const math::Double3 b = PointDirection(to->second);
                const f64 angle = std::acos(std::clamp(math::Dot(a, b), -1.0, 1.0));
                const std::size_t n = static_cast<std::size_t>(count);
                for (std::size_t i = 0; i < n; ++i)
                {
                    const f64 u = static_cast<f64>(i) / static_cast<f64>(n - 1U);
                    if (angle < 1.0e-9)
                    {
                        directions.push_back(a);
                        continue;
                    }
                    const f64 sa = std::sin((1.0 - u) * angle) / std::sin(angle);
                    const f64 sb = std::sin(u * angle) / std::sin(angle);
                    directions.push_back(a * sa + b * sb);
                }
            }

            rpc::Value::Array samples;
            samples.reserve(directions.size());
            for (const auto& direction : directions)
                samples.push_back(SamplePoint(bound, direction, footprint, tectonics, seaLevel));
            return rpc::Value(rpc::Value::Object{
                {"terrain", bound.object.ToString()},
                {"planet_radius_meters", bound.planet.radiusMeters},
                {"sea_level_meters", seaLevel},
                {"footprint_meters", footprint},
                {"samples", std::move(samples)}});
        });

    dispatcher.Register(
        {
            .name = "studio.process_info",
            .description =
                "Which Studio process answers: pid, seconds since the process started "
                "answering and the number of composed terrains. A hot generation handoff or a "
                "project relaunch changes the pid; poll this after project.open or a rebuild "
                "until the pid changes and terrain_count is non-zero instead of sleeping. "
                "Read-only.",
            .mutating = false
        },
        [&session, started](const rpc::Value&)
        {
            const f64 uptime =
                std::chrono::duration<f64>(std::chrono::steady_clock::now() - started).count();
            return rpc::Value(rpc::Value::Object{
                {"pid", ProcessId()},
                {"uptime_seconds", uptime},
                {"terrain_count",
                 static_cast<i64>(session.World().Surfaces().TerrainBodies().size())}});
        });
}
} // namespace orbit::studio_session
