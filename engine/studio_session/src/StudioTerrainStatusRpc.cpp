#include <orbit/studio_session/StudioTerrainStatusRpc.hpp>

#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/terrain_bake/TerrainBakeService.hpp>
#include <orbit/studio_session/StudioTerrainAuthoringInvalidation.hpp>
#include <orbit/studio_session/StudioTerrainServiceStatus.hpp>
#include <orbit/studio_session/StudioTerrainTectonicsProbe.hpp>
#include <orbit/world/PlanetTileNeighborhood.hpp>
#include <orbit/terrain/TectonicFieldDesc.hpp>
#include <orbit/terrain_erosion/RiverNetwork.hpp>
#include <orbit/terrain_water/LakeWater.hpp>
#include <orbit/world/PlanetTileNeighborhood.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace orbit::studio_session
{
namespace
{
[[nodiscard]] const rpc::Value::Object& RequireObject(
    const rpc::Value& params)
{
    if (!params.IsObject())
    {
        throw rpc::Error(-32602, "Params must be an object.");
    }

    return params.AsObject();
}

[[nodiscard]] std::string RequireString(
    const rpc::Value::Object& object,
    const std::string_view key)
{
    const auto found = object.find(key);

    if (found == object.end() ||
        !found->second.IsString() ||
        found->second.AsString().empty())
    {
        throw rpc::Error(
            -32602,
            std::string(key) + " must be a non-empty string.");
    }

    return found->second.AsString();
}

void QueueGlobalProcessSettingsInvalidation(
    StudioSession& session,
    const scene::ObjectId terrain)
{
    const auto body = session.World().Surfaces().BodyForTerrainObject(terrain);
    if (!body.has_value()) throw rpc::Error(-32602, "terrain is not bound to a spherical body.");
    const auto planet = session.World().Surfaces().Registry().SphericalPlanetDefinition(*body);
    if (!planet.has_value()) throw rpc::Error(-32602, "terrain body has no spherical planet definition.");
    session.QueueTerrainInvalidation({
        .kind = terrain_dependency::TerrainChangeKind::ProcessSettings,
        .scope = {.planet = planet->id, .global = true}});
}

[[nodiscard]] rpc::Value CacheStatsToRpc(
    const terrain_gpu::PersistentGpuTerrainCacheStats& stats)
{
    const f64 lookups =
        static_cast<f64>(stats.hits + stats.misses);

    return rpc::Value(
        rpc::Value::Object{
            {"hits", static_cast<i64>(stats.hits)},
            {"misses", static_cast<i64>(stats.misses)},
            {"generations", static_cast<i64>(stats.generations)},
            {"insertions", static_cast<i64>(stats.insertions)},
            {"evictions", static_cast<i64>(stats.evictions)},
            {"resident_pages", static_cast<i64>(stats.residentPages)},
            {"resident_bytes", static_cast<i64>(stats.residentBytes)},
            {"hit_rate_percent",
             lookups > 0.0
                 ? 100.0 * static_cast<f64>(stats.hits) / lookups
                 : 0.0}
        });
}

[[nodiscard]] rpc::Value TectonicsToRpc(
    const terrain::TectonicFieldDesc& settings)
{
    return rpc::Value(rpc::Value::Object{
        {"seed", static_cast<i64>(settings.seed)},
        {"plate_count", static_cast<i64>(settings.plateCount)},
        {"plate_irregularity", settings.plateIrregularity},
        {"continental_fraction", settings.continentalPlateFraction},
        {"continent_influence", settings.tectonicContinentInfluence},
        {"boundary_width", settings.boundaryWidthDot},
        {"minimum_angular_speed", settings.minPlateAngularSpeed},
        {"maximum_angular_speed", settings.maxPlateAngularSpeed},
        {"convergent_uplift_meters", settings.convergenceUpliftMeters},
        {"oceanic_collision_scale", settings.oceanicConvergenceScale},
        {"hotspot_count", static_cast<i64>(settings.hotspotCount)},
        {"hotspot_age_steps", static_cast<i64>(settings.hotspotAgeSteps)},
        {"hotspot_relief_meters", settings.hotspotBaseReliefMeters},
        {"hotspot_age_decay", settings.hotspotAgeDecay},
        {"hotspot_chain_spacing_meters", settings.hotspotChainSpacingMeters},
        {"hotspot_core_radius_meters", settings.hotspotCoreRadiusMeters},
        {"plate_size_variance", settings.plateSizeVarianceDot},
        {"continental_crust_bias_meters", settings.continentalPlateBiasMeters},
        {"oceanic_crust_bias_meters", settings.oceanicPlateBiasMeters},
        {"convergence_reference_speed", settings.convergenceReferenceSpeed},
        {"transform_reference_speed", settings.transformReferenceSpeed},
        {"hotspot_radius_growth_per_age", settings.hotspotRadiusGrowthPerAge},
        {"belt_ridge_relief", settings.beltRidgeRelief}
    });
}

void ReadTectonicsPatch(
    const rpc::Value::Object& values,
    terrain::TectonicFieldDesc& settings)
{
    static constexpr std::array<std::string_view, 23> kSettings{
        "seed", "plate_count", "plate_irregularity", "plate_size_variance",
        "continental_fraction", "continental_crust_bias_meters",
        "oceanic_crust_bias_meters", "continent_influence", "boundary_width",
        "minimum_angular_speed", "maximum_angular_speed",
        "convergence_reference_speed", "transform_reference_speed",
        "convergent_uplift_meters", "oceanic_collision_scale", "hotspot_count",
        "hotspot_age_steps", "hotspot_relief_meters", "hotspot_age_decay",
        "hotspot_chain_spacing_meters", "hotspot_core_radius_meters",
        "hotspot_radius_growth_per_age", "belt_ridge_relief"};
    for (const auto& [key, value] : values)
    {
        static_cast<void>(value);
        if (key != "terrain" &&
            std::find(kSettings.begin(), kSettings.end(), key) == kSettings.end())
            throw rpc::Error(-32602, "Unknown tectonics setting: " + key + ".");
    }

    const auto number = [&values](const char* key, f64& target)
    {
        const auto found = values.find(key);
        if (found == values.end()) return;
        if (!found->second.IsNumber())
            throw rpc::Error(-32602, std::string(key) + " must be a number.");
        target = found->second.AsNumber();
    };
    const auto integer = [&values](const char* key, i64& target)
    {
        const auto found = values.find(key);
        if (found == values.end()) return;
        if (!found->second.IsInteger())
            throw rpc::Error(-32602, std::string(key) + " must be an integer.");
        target = found->second.AsInteger();
    };

    i64 seed = static_cast<i64>(settings.seed);
    i64 plateCount = static_cast<i64>(settings.plateCount);
    i64 hotspotCount = static_cast<i64>(settings.hotspotCount);
    i64 hotspotAgeSteps = static_cast<i64>(settings.hotspotAgeSteps);
    integer("seed", seed);
    integer("plate_count", plateCount);
    integer("hotspot_count", hotspotCount);
    integer("hotspot_age_steps", hotspotAgeSteps);
    if (seed < 0 || plateCount < 0 || hotspotCount < 0 || hotspotAgeSteps < 0)
        throw rpc::Error(-32602, "Tectonic integer settings must be non-negative.");
    settings.seed = static_cast<u64>(seed);
    settings.plateCount = static_cast<u32>(plateCount);
    settings.hotspotCount = static_cast<u32>(hotspotCount);
    settings.hotspotAgeSteps = static_cast<u32>(hotspotAgeSteps);

    number("plate_irregularity", settings.plateIrregularity);
    number("continental_fraction", settings.continentalPlateFraction);
    number("continent_influence", settings.tectonicContinentInfluence);
    number("boundary_width", settings.boundaryWidthDot);
    number("minimum_angular_speed", settings.minPlateAngularSpeed);
    number("maximum_angular_speed", settings.maxPlateAngularSpeed);
    number("convergent_uplift_meters", settings.convergenceUpliftMeters);
    number("oceanic_collision_scale", settings.oceanicConvergenceScale);
    number("hotspot_relief_meters", settings.hotspotBaseReliefMeters);
    number("hotspot_age_decay", settings.hotspotAgeDecay);
    number("hotspot_chain_spacing_meters", settings.hotspotChainSpacingMeters);
    number("hotspot_core_radius_meters", settings.hotspotCoreRadiusMeters);
    number("plate_size_variance", settings.plateSizeVarianceDot);
    number("continental_crust_bias_meters", settings.continentalPlateBiasMeters);
    number("oceanic_crust_bias_meters", settings.oceanicPlateBiasMeters);
    number("convergence_reference_speed", settings.convergenceReferenceSpeed);
    number("transform_reference_speed", settings.transformReferenceSpeed);
    number("hotspot_radius_growth_per_age", settings.hotspotRadiusGrowthPerAge);
}

[[nodiscard]] rpc::Value TectonicProbeToRpc(
    const StudioTectonicsProbe& probe)
{
    number("belt_ridge_relief", settings.beltRidgeRelief);
    const auto& s = probe.structure;
    return rpc::Value(rpc::Value::Object{
        {"latitude_degrees", probe.latitudeDegrees},
        {"longitude_degrees", probe.longitudeDegrees},
        {"plate_id", static_cast<i64>(s.plateId)},
        {"neighbour_plate_id", static_cast<i64>(s.neighbourPlateId)},
        {"plate_type", s.continental ? "continental" : "oceanic"},
        {"neighbour_plate_type", s.neighbourContinental ? "continental" : "oceanic"},
        {"boundary_type", std::string(terrain::TectonicBoundaryTypeName(s.boundaryType))},
        {"boundary_strength", s.boundaryStrength},
        {"convergence", s.convergence},
        {"divergence", s.divergence},
        {"transform", s.transform},
        {"plate_speed_meters_per_unit", s.plateSpeedMetersPerUnit},
        {"crust_thickness_km", s.crustThicknessKm},
        {"crust_age", s.crustAge},
        {"geological_age", s.geologicalAge},
        {"uplift_meters", s.upliftMeters},
        {"subsidence_meters", s.subsidenceMeters},
        {"stress", s.stress},
        {"volcanism", s.volcanism}});
}

[[nodiscard]] rpc::Value CouplingToRpc(
    const terrain_erosion::StreamPowerErosionConfig& config)
{
    return rpc::Value(rpc::Value::Object{
        {"age_erodibility_gain", config.ageErodibilityGain},
        {"age_uplift_decay", config.ageUpliftDecay},
        {"tectonic_drainage_guidance", config.tectonicDrainageGuidance}});
}

[[nodiscard]] std::string HashText(const u64 value)
{
    return std::format("{:016x}", value);
}

[[nodiscard]] rpc::Value BakeStatusToRpc(
    const scene::ObjectId terrain,
    const terrain_bake::BakeStatus& status)
{
    return rpc::Value(rpc::Value::Object{
        {"terrain", terrain.ToString()},
        {"state", std::string(terrain_bake::BakeStateName(status.state))},
        {"progress", static_cast<f64>(status.progress)},
        {"stale", status.state == terrain_bake::BakeState::Stale},
        {"resolution", static_cast<i64>(status.settings.resolution)},
        {"auto_rebake", status.settings.autoRebake},
        {"active_resolution", static_cast<i64>(status.activeResolution)},
        {"active_bytes", static_cast<i64>(status.activeBytes)},
        {"rivers_active", status.riversActive},
        {"river_nodes", static_cast<i64>(status.riverNodes)},
        {"river_segments", static_cast<i64>(status.riverSegments)},
        {"river_bytes", static_cast<i64>(status.riverBytes)},
        {"current_river_hash", HashText(status.currentRiverHash)},
        {"active_river_hash", HashText(status.activeRiverHash)},
        {"current_recipe_hash", HashText(status.currentRecipeHash)},
        {"active_recipe_hash", HashText(status.activeRecipeHash)},
        {"last_bake_seconds", status.lastBakeSeconds},
        {"error", status.error},
        {"path", status.path.generic_string()}});
}

[[nodiscard]] scene::ObjectId RequireTerrainObject(const rpc::Value::Object& values)
{
    const auto terrain = scene::ObjectId::Parse(RequireString(values, "terrain"));
    if (!terrain.has_value())
        throw rpc::Error(-32602, "terrain must be a terrain object id.");
    return *terrain;
}

[[nodiscard]] editor_model::SurfaceAuthoringModel SurfaceModel(
    StudioSession& session)
{
    auto& world = session.World();
    return editor_model::SurfaceAuthoringModel(
        world.Objects(), world.Commands(), world.Selection());
}
} // namespace

void RegisterStudioTerrainStatusRpc(
    rpc::Dispatcher& dispatcher,
    StudioSession& session)
{
    dispatcher.Register(
        {
            .name = "terrain.tectonics_get",
            .description =
                "Returns the persisted procedural tectonics recipe for a terrain surface. "
                "The recipe is shared with Planet toolbar authoring and drives the same "
                "analytic plate field used by terrain generation.",
            .mutating = false
        },
        [&session](const rpc::Value& params)
        {
            const auto& values = RequireObject(params);
            const auto terrain = scene::ObjectId::Parse(RequireString(values, "terrain"));
            if (!terrain.has_value()) throw rpc::Error(-32602, "terrain must be a terrain object id.");
            try
            {
                return rpc::Value(rpc::Value::Object{
                    {"terrain", terrain->ToString()},
                    {"settings", TectonicsToRpc(SurfaceModel(session).Tectonics(*terrain))}
                });
            }
            catch (const rpc::Error&) { throw; }
            catch (const std::exception& exception)
            {
                throw rpc::Error(1004, exception.what());
            }
        });

    dispatcher.Register(
        {
            .name = "terrain.tectonics_set",
            .description =
                "Updates any supplied fields of a terrain surface's procedural tectonics recipe "
                "as one undoable edit. Omitted fields retain their current values. This is the "
                "same model operation used by the Planet toolbar.",
            .mutating = true
        },
        [&session](const rpc::Value& params)
        {
            const auto& values = RequireObject(params);
            const auto terrain = scene::ObjectId::Parse(RequireString(values, "terrain"));
            if (!terrain.has_value()) throw rpc::Error(-32602, "terrain must be a terrain object id.");
            try
            {
                auto model = SurfaceModel(session);
                auto settings = model.Tectonics(*terrain);
                ReadTectonicsPatch(values, settings);
                model.SetTectonics(*terrain, settings);
                return rpc::Value(rpc::Value::Object{
                    {"terrain", terrain->ToString()},
                    {"settings", TectonicsToRpc(model.Tectonics(*terrain))}
                });
            }
            catch (const rpc::Error&) { throw; }
            catch (const std::invalid_argument& exception)
            {
                throw rpc::Error(-32602, exception.what());
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(1004, exception.what());
            }
        });

    dispatcher.Register(
        {
            .name = "terrain.hydrology_get",
            .description =
                "Returns persisted runoff, soil water, and simulation-time seasonal rainfall controls.",
            .mutating = false
        },
        [&session](const rpc::Value& params)
        {
            const auto& values = RequireObject(params);
            const auto terrain = scene::ObjectId::Parse(RequireString(values, "terrain"));
            if (!terrain.has_value()) throw rpc::Error(-32602, "terrain must be a terrain object id.");
            try
            {
                const auto settings = SurfaceModel(session).ProcessSettings(*terrain).hydraulic;
                return rpc::Value(rpc::Value::Object{
                    {"terrain", terrain->ToString()},
                    {"rainfall_meters_per_second", settings.rainfallMetersPerSecond},
                    {"infiltration_meters_per_second", settings.infiltrationMetersPerSecond},
                    {"moisture_capacity_depth_meters", settings.moistureCapacityDepthMeters},
                    {"evaporation_rate_per_second", settings.evaporationRatePerSecond},
                    {"seasonal_rainfall_amplitude", settings.seasonalRainfallAmplitude},
                    {"seasonal_rainfall_period_seconds", settings.seasonalRainfallPeriodSeconds},
                    {"seasonal_rainfall_phase_radians", settings.seasonalRainfallPhaseRadians},
                    {"seasonal_update_bins_per_cycle", i64{12}},
                    {"precipitation_source", "static terrain climate field"}
                });
            }
            catch (const std::exception& exception) { throw rpc::Error(1004, exception.what()); }
        });

    dispatcher.Register(
        {
            .name = "terrain.hydrology_set",
            .description =
                "Updates supplied runoff, soil water, and seasonal rainfall controls as one undoable edit through SurfaceAuthoringModel.",
            .mutating = true
        },
        [&session](const rpc::Value& params)
        {
            const auto& values = RequireObject(params);
            const auto terrain = scene::ObjectId::Parse(RequireString(values, "terrain"));
            if (!terrain.has_value()) throw rpc::Error(-32602, "terrain must be a terrain object id.");
            static constexpr std::array<std::string_view, 7> fields{
                "rainfall_meters_per_second", "infiltration_meters_per_second",
                "moisture_capacity_depth_meters", "evaporation_rate_per_second",
                "seasonal_rainfall_amplitude", "seasonal_rainfall_period_seconds",
                "seasonal_rainfall_phase_radians"};
            for (const auto& [key, value] : values)
            {
                static_cast<void>(value);
                if (key != "terrain" && std::find(fields.begin(), fields.end(), key) == fields.end())
                    throw rpc::Error(-32602, "Unknown hydrology setting: " + key + ".");
            }
            try
            {
                auto model = SurfaceModel(session);
                auto settings = model.ProcessSettings(*terrain);
                const auto number = [&values](const char* key, f64& target)
                {
                    const auto found = values.find(key);
                    if (found == values.end()) return;
                    if (!found->second.IsNumber()) throw rpc::Error(-32602, std::string(key) + " must be a number.");
                    target = found->second.AsNumber();
                };
                number("rainfall_meters_per_second", settings.hydraulic.rainfallMetersPerSecond);
                number("infiltration_meters_per_second", settings.hydraulic.infiltrationMetersPerSecond);
                number("moisture_capacity_depth_meters", settings.hydraulic.moistureCapacityDepthMeters);
                number("evaporation_rate_per_second", settings.hydraulic.evaporationRatePerSecond);
                number("seasonal_rainfall_amplitude", settings.hydraulic.seasonalRainfallAmplitude);
                number("seasonal_rainfall_period_seconds", settings.hydraulic.seasonalRainfallPeriodSeconds);
                number("seasonal_rainfall_phase_radians", settings.hydraulic.seasonalRainfallPhaseRadians);
                model.SetProcessSettings(*terrain, settings);
                QueueGlobalProcessSettingsInvalidation(session, *terrain);
                return rpc::Value(rpc::Value::Object{
                    {"terrain", terrain->ToString()},
                    {"rainfall_meters_per_second", settings.hydraulic.rainfallMetersPerSecond},
                    {"infiltration_meters_per_second", settings.hydraulic.infiltrationMetersPerSecond},
                    {"moisture_capacity_depth_meters", settings.hydraulic.moistureCapacityDepthMeters},
                    {"evaporation_rate_per_second", settings.hydraulic.evaporationRatePerSecond},
                    {"seasonal_rainfall_amplitude", settings.hydraulic.seasonalRainfallAmplitude},
                    {"seasonal_rainfall_period_seconds", settings.hydraulic.seasonalRainfallPeriodSeconds},
                    {"seasonal_rainfall_phase_radians", settings.hydraulic.seasonalRainfallPhaseRadians},
                    {"seasonal_update_bins_per_cycle", i64{12}},
                    {"precipitation_source", "static terrain climate field"}
                });
            }
            catch (const rpc::Error&) { throw; }
            catch (const std::invalid_argument& exception) { throw rpc::Error(-32602, exception.what()); }
            catch (const std::exception& exception) { throw rpc::Error(1004, exception.what()); }
        });

    dispatcher.Register(
        {
            .name = "terrain.drainage_spline_add",
            .description =
                "Creates a persisted terrain drainage-guidance spline. M09 may prefer its corridor only when routing downhill; M16 derives the resulting river graph.",
            .mutating = true
        },
        [&session](const rpc::Value& params)
        {
            const auto& values = RequireObject(params);
            const auto terrain = scene::ObjectId::Parse(RequireString(values, "terrain"));
            if (!terrain.has_value()) throw rpc::Error(-32602, "terrain must be a terrain object id.");
            static constexpr std::array<std::string_view, 5> fields{
                "terrain", "points", "half_width_meters", "falloff_meters", "guidance"};
            for (const auto& [key, value] : values)
            {
                static_cast<void>(value);
                if (std::find(fields.begin(), fields.end(), key) == fields.end())
                    throw rpc::Error(-32602, "Unknown drainage spline setting: " + key + ".");
            }

            const auto pointsValue = values.find("points");
            if (pointsValue == values.end() || !pointsValue->second.IsArray())
                throw rpc::Error(-32602, "points must be an array of three-number directions.");
            std::vector<math::Double3> points;
            for (const auto& pointValue : pointsValue->second.AsArray())
            {
                if (!pointValue.IsArray() || pointValue.AsArray().size() != 3U ||
                    !pointValue.AsArray()[0].IsNumber() ||
                    !pointValue.AsArray()[1].IsNumber() ||
                    !pointValue.AsArray()[2].IsNumber())
                    throw rpc::Error(-32602, "Each point must be [x, y, z] with numeric unit-direction components.");
                points.push_back({
                    pointValue.AsArray()[0].AsNumber(),
                    pointValue.AsArray()[1].AsNumber(),
                    pointValue.AsArray()[2].AsNumber()});
            }

            const auto number = [&values](const char* key) -> f64
            {
                const auto found = values.find(key);
                if (found == values.end() || !found->second.IsNumber())
                    throw rpc::Error(-32602, std::string(key) + " must be a number.");
                return found->second.AsNumber();
            };
            const f64 halfWidth = number("half_width_meters");
            const f64 falloff = number("falloff_meters");
            const f64 guidance = number("guidance");

            try
            {
                const auto body = session.World().Surfaces().BodyForTerrainObject(*terrain);
                if (!body.has_value()) throw rpc::Error(-32602, "terrain is not bound to a spherical body.");
                const auto planet = session.World().Surfaces().Registry().SphericalPlanetDefinition(*body);
                if (!planet.has_value()) throw rpc::Error(-32602, "terrain body has no spherical planet definition.");

                std::vector<terrain_dependency::TerrainInvalidationRequest> invalidations;
                const auto runtime = session.TerrainRuntime().Capture("studio.primary");
                if (runtime.has_value() && runtime->body == *body)
                {
                    invalidations = BuildTerrainAuthoringInvalidations(
                        *planet,
                        points,
                        halfWidth + falloff,
                        runtime->physicalPageLevel,
                        3U);
                }

                auto model = SurfaceModel(session);
                const auto constraint = model.AddDrainageSpline(
                    *terrain, points, halfWidth, falloff, guidance);
                model.SelectObject(constraint);
                session.QueueTerrainInvalidations(invalidations);
                return rpc::Value(rpc::Value::Object{
                    {"terrain", terrain->ToString()},
                    {"constraint", constraint.ToString()},
                    {"control_point_count", static_cast<i64>(points.size())},
                    {"guidance", guidance},
                    {"invalidations_queued", static_cast<i64>(invalidations.size())}
                });
            }
            catch (const rpc::Error&) { throw; }
            catch (const std::invalid_argument& exception) { throw rpc::Error(-32602, exception.what()); }
            catch (const std::exception& exception) { throw rpc::Error(1004, exception.what()); }
        });

    dispatcher.Register(
        {
            .name = "terrain.rivers_get",
            .description = "Returns the persisted mountain-fed river recipe shared by the Planet toolbar and terrain generation.",
            .mutating = false
        },
        [&session](const rpc::Value& params)
        {
            const auto& values = RequireObject(params);
            const auto terrain = scene::ObjectId::Parse(RequireString(values, "terrain"));
            if (!terrain.has_value()) throw rpc::Error(-32602, "terrain must be a terrain object id.");
            try
            {
                const auto settings = SurfaceModel(session).ProcessSettings(*terrain);
                return rpc::Value(rpc::Value::Object{
                    {"terrain", terrain->ToString()},
                    {"enabled", settings.riversEnabled},
                    {"maximum_node_spacing_meters", settings.rivers.maximumNodeSpacingMeters},
                    {"minimum_drainage_area_square_meters", settings.rivers.minimumDrainageAreaSquareMeters},
                    {"minimum_discharge_cubic_meters_per_second", settings.rivers.minimumDischargeCubicMetersPerSecond},
                    {"reference_discharge_cubic_meters_per_second", settings.rivers.referenceDischargeCubicMetersPerSecond},
                    {"base_channel_width_meters", settings.rivers.baseChannelWidthMeters},
                    {"minimum_channel_width_meters", settings.rivers.minimumChannelWidthMeters},
                    {"maximum_channel_width_meters", settings.rivers.maximumChannelWidthMeters},
                    {"width_discharge_exponent", settings.rivers.widthDischargeExponent},
                    {"base_channel_depth_meters", settings.rivers.baseChannelDepthMeters},
                    {"minimum_channel_depth_meters", settings.rivers.minimumChannelDepthMeters},
                    {"maximum_channel_depth_meters", settings.rivers.maximumChannelDepthMeters},
                    {"depth_discharge_exponent", settings.rivers.depthDischargeExponent},
                    {"meanders", settings.rivers.enableMeanders},
                    {"cutoffs", settings.rivers.enableCutoffs},
                    {"meander_iterations", static_cast<i64>(settings.rivers.meanderIterations)},
                    {"meander_time_step", settings.rivers.meanderTimeStep},
                    {"curvature_migration_rate", settings.rivers.curvatureMigrationRate},
                    {"seed_migration_rate", settings.rivers.deterministicSeedMigrationRate},
                    {"maximum_centerline_offset_widths", settings.rivers.maximumCenterlineOffsetWidths},
                    {"minimum_cutoff_path_nodes", static_cast<i64>(settings.rivers.minimumCutoffPathNodes)},
                    {"cutoff_distance_widths", settings.rivers.cutoffDistanceWidths}
                });
            }
            catch (const std::exception& exception) { throw rpc::Error(1004, exception.what()); }
        });

    dispatcher.Register(
        {
            .name = "terrain.tectonics_sample",
            .description =
                "Samples the planet structural layer (plate, boundary type/strength, crust "
                "thickness and age, geological age, uplift, subsidence, stress, volcanism) "
                "at latitude_degrees/longitude_degrees, or under the viewport observer when "
                "omitted. Read-only and derived from the persisted tectonics recipe.",
            .mutating = false
        },
        [&session](const rpc::Value& params)
        {
            const auto& values = RequireObject(params);
            const std::string viewport = values.contains("viewport")
                ? RequireString(values, "viewport")
                : "studio.primary";
            const auto latitude = values.find("latitude_degrees");
            const auto longitude = values.find("longitude_degrees");
            std::optional<math::Double3> direction;
            if (latitude != values.end() || longitude != values.end())
            {
                if (latitude == values.end() || longitude == values.end() ||
                    !latitude->second.IsNumber() || !longitude->second.IsNumber())
                    throw rpc::Error(
                        -32602,
                        "latitude_degrees and longitude_degrees must be numbers given together.");
                if (std::abs(latitude->second.AsNumber()) > 90.0)
                    throw rpc::Error(-32602, "latitude_degrees must be within -90..90.");
                direction = TectonicsDirectionFromLatLon(
                    latitude->second.AsNumber(), longitude->second.AsNumber());
            }
            const auto probe = ProbeTectonicStructure(session, viewport, direction);
            if (!probe.has_value())
                throw rpc::Error(1004, "No analytic terrain runtime is available for the viewport.");
            auto result = TectonicProbeToRpc(*probe);
            result.AsObject().emplace("viewport", viewport);
            return result;
        });

    dispatcher.Register(
        {
            .name = "terrain.erosion_coupling_get",
            .description =
                "Returns how the planet structural layer drives erosion and watersheds: "
                "geological-age erodibility gain, age-driven uplift decay and tectonic "
                "drainage guidance (routing is steered from uplifting belts toward basins).",
            .mutating = false
        },
        [&session](const rpc::Value& params)
        {
            const auto& values = RequireObject(params);
            const auto terrain = scene::ObjectId::Parse(RequireString(values, "terrain"));
            if (!terrain.has_value()) throw rpc::Error(-32602, "terrain must be a terrain object id.");
            try
            {
                auto result = CouplingToRpc(SurfaceModel(session).ProcessSettings(*terrain).streamPower);
                result.AsObject().emplace("terrain", terrain->ToString());
                return result;
            }
            catch (const std::exception& exception) { throw rpc::Error(1004, exception.what()); }
        });

    dispatcher.Register(
        {
            .name = "terrain.erosion_coupling_set",
            .description =
                "Updates supplied geological-age and tectonic drainage coupling controls as one "
                "undoable edit through SurfaceAuthoringModel and queues global terrain regeneration, "
                "exactly like the Planet Tectonics menu.",
            .mutating = true
        },
        [&session](const rpc::Value& params)
        {
            const auto& values = RequireObject(params);
            const auto terrain = scene::ObjectId::Parse(RequireString(values, "terrain"));
            if (!terrain.has_value()) throw rpc::Error(-32602, "terrain must be a terrain object id.");
            static constexpr std::array<std::string_view, 3> fields{
                "age_erodibility_gain", "age_uplift_decay", "tectonic_drainage_guidance"};
            for (const auto& [key, value] : values)
            {
                static_cast<void>(value);
                if (key != "terrain" && std::find(fields.begin(), fields.end(), key) == fields.end())
                    throw rpc::Error(-32602, "Unknown erosion coupling setting: " + key + ".");
            }
            try
            {
                auto model = SurfaceModel(session);
                auto settings = model.ProcessSettings(*terrain);
                const auto number = [&values](const char* key, f64& target)
                {
                    const auto found = values.find(key);
                    if (found == values.end()) return;
                    if (!found->second.IsNumber()) throw rpc::Error(-32602, std::string(key) + " must be a number.");
                    target = found->second.AsNumber();
                };
                number("age_erodibility_gain", settings.streamPower.ageErodibilityGain);
                number("age_uplift_decay", settings.streamPower.ageUpliftDecay);
                number("tectonic_drainage_guidance", settings.streamPower.tectonicDrainageGuidance);
                model.SetProcessSettings(*terrain, settings);
                QueueGlobalProcessSettingsInvalidation(session, *terrain);
                auto result = CouplingToRpc(model.ProcessSettings(*terrain).streamPower);
                result.AsObject().emplace("terrain", terrain->ToString());
                return result;
            }
            catch (const rpc::Error&) { throw; }
            catch (const std::invalid_argument& exception) { throw rpc::Error(-32602, exception.what()); }
            catch (const std::exception& exception) { throw rpc::Error(1004, exception.what()); }
        });

    dispatcher.Register(
        {
            .name = "terrain.bake_status",
            .description =
                "Planet bake state for a terrain surface: state (none, baking, ready, stale, failed), "
                "progress, resolution, active bake size, recipe hashes (stale means the recipe changed "
                "since the active bake), the bake file path and any error. Terrain generation samples the "
                "bake; it never evaluates the plate model itself.",
            .mutating = false
        },
        [&session](const rpc::Value& params)
        {
            const auto terrain = RequireTerrainObject(RequireObject(params));
            const auto status = session.TerrainBake().Status(terrain);
            if (!status.has_value())
                throw rpc::Error(1004, "The terrain surface has no bake service (is the world open and the body composed?).");
            return BakeStatusToRpc(terrain, *status);
        });

    dispatcher.Register(
        {
            .name = "terrain.bake_start",
            .description =
                "Starts a background planet bake for the terrain surface now (cancels one in flight). "
                "Optional resolution is texels per cube-face edge, 16-2048. The running bake keeps driving "
                "terrain until the new one finishes and validates; a failed bake leaves it untouched.",
            .mutating = true
        },
        [&session](const rpc::Value& params)
        {
            const auto& values = RequireObject(params);
            const auto terrain = RequireTerrainObject(values);
            std::optional<u32> resolution;
            if (const auto found = values.find("resolution"); found != values.end())
            {
                if (!found->second.IsInteger() || found->second.AsInteger() < 16 ||
                    found->second.AsInteger() > 2048)
                    throw rpc::Error(-32602, "resolution must be an integer from 16 to 2048.");
                resolution = static_cast<u32>(found->second.AsInteger());
            }
            // The resolution belongs to the persisted policy; applying it first
            // keeps the next tick from re-applying the old one and rebaking.
            if (resolution.has_value())
            {
                try
                {
                    auto model = SurfaceModel(session);
                    auto settings = model.ProcessSettings(terrain);
                    if (settings.bake.resolution != *resolution)
                    {
                        settings.bake.resolution = *resolution;
                        model.SetProcessSettings(terrain, settings);
                    }
                }
                catch (const std::exception& exception)
                {
                    throw rpc::Error(1004, exception.what());
                }
            }
            if (!session.TerrainBake().StartBake(terrain, resolution))
                throw rpc::Error(1004, "The bake could not be started for this terrain surface.");
            return BakeStatusToRpc(terrain, *session.TerrainBake().Status(terrain));
        });

    dispatcher.Register(
        {
            .name = "terrain.bake_cancel",
            .description = "Cancels a running planet bake. The active bake is untouched and no automatic rebake starts until the recipe changes or terrain.bake_start is called.",
            .mutating = true
        },
        [&session](const rpc::Value& params)
        {
            const auto terrain = RequireTerrainObject(RequireObject(params));
            session.TerrainBake().Cancel(terrain);
            const auto status = session.TerrainBake().Status(terrain);
            if (!status.has_value())
                throw rpc::Error(1004, "The terrain surface has no bake service.");
            return BakeStatusToRpc(terrain, *status);
        });

    dispatcher.Register(
        {
            .name = "terrain.bake_set",
            .description =
                "Updates the persisted planet bake policy (resolution 16-2048 and auto_rebake) as one "
                "undoable edit through SurfaceAuthoringModel, the same operation as the Planet Tectonics "
                "menu's Planet Bake section. A resolution change makes the bake stale; it rebakes when "
                "auto_rebake is on or terrain.bake_start is called.",
            .mutating = true
        },
        [&session](const rpc::Value& params)
        {
            const auto& values = RequireObject(params);
            const auto terrain = RequireTerrainObject(values);
            for (const auto& [key, value] : values)
            {
                static_cast<void>(value);
                if (key != "terrain" && key != "resolution" && key != "auto_rebake")
                    throw rpc::Error(-32602, "Unknown bake setting: " + key + ".");
            }
            try
            {
                auto model = SurfaceModel(session);
                auto settings = model.ProcessSettings(terrain);
                if (const auto found = values.find("resolution"); found != values.end())
                {
                    if (!found->second.IsInteger())
                        throw rpc::Error(-32602, "resolution must be an integer.");
                    const i64 resolution = found->second.AsInteger();
                    if (resolution < 16 || resolution > 2048)
                        throw rpc::Error(-32602, "resolution must be from 16 to 2048.");
                    settings.bake.resolution = static_cast<u32>(resolution);
                }
                if (const auto found = values.find("auto_rebake"); found != values.end())
                {
                    if (!found->second.IsBool())
                        throw rpc::Error(-32602, "auto_rebake must be a boolean.");
                    settings.bake.autoRebake = found->second.AsBool();
                }
                model.SetProcessSettings(terrain, settings);
                const auto status = session.TerrainBake().Status(terrain);
                if (status.has_value())
                    return BakeStatusToRpc(terrain, *status);
                return rpc::Value(rpc::Value::Object{
                    {"terrain", terrain.ToString()},
                    {"resolution", static_cast<i64>(settings.bake.resolution)},
                    {"auto_rebake", settings.bake.autoRebake}});
            }
            catch (const rpc::Error&) { throw; }
            catch (const std::invalid_argument& exception) { throw rpc::Error(-32602, exception.what()); }
            catch (const std::exception& exception) { throw rpc::Error(1004, exception.what()); }
        });

    dispatcher.Register(
        {
            .name = "terrain.rivers_nearby",
            .description = "Lists generated river nodes on already-built physical terrain pages near the active viewport observer. Results are derived read-only data; empty pages are reported as pending rather than synthesized.",
            .mutating = false
        },
        [&session](const rpc::Value& params)
        {
            const auto& values = RequireObject(params);
            const std::string viewport = values.contains("viewport")
                ? RequireString(values, "viewport")
                : "studio.primary";
            const auto terrain = scene::ObjectId::Parse(
                RequireString(values, "terrain"));
            if (!terrain.has_value())
                throw rpc::Error(-32602, "terrain must be a terrain object id.");

            const auto runtime = session.TerrainRuntime().Capture(viewport);
            if (!runtime.has_value())
                throw rpc::Error(1004, "No active terrain viewport runtime is available.");
            const auto body = session.World().Surfaces().BodyForTerrainObject(*terrain);
            if (!body.has_value() || *body != runtime->body)
                throw rpc::Error(-32602, "terrain does not match the active viewport body.");

            const auto addresses = world::TileNeighborhood(
                runtime->observerPhysicalPage.tile, 1U);
            rpc::Value::Array pages;
            rpc::Value::Array nodes;
            rpc::Value::Array segments;
            rpc::Value::Array boundaryLinks;
            u64 pendingPages = 0U;
            u64 boundaryLinkCount = 0U;
            for (const auto& tile : addresses)
            {
                const terrain::PhysicalTerrainPageAddress address{
                    .planet = runtime->planet.id,
                    .tile = tile};
                const auto page = session.TerrainPhysicalPages().Find(address);
                if (page == nullptr || page->rivers == nullptr)
                {
                    ++pendingPages;
                    continue;
                }
                pages.emplace_back(rpc::Value::Object{
                    {"face", static_cast<i64>(tile.face)},
                    {"level", static_cast<i64>(tile.level)},
                    {"x", static_cast<i64>(tile.x)},
                    {"y", static_cast<i64>(tile.y)},
                    {"node_count", static_cast<i64>(page->rivers->nodes.size())},
                    {"segment_count", static_cast<i64>(page->rivers->segments.size())},
                    {"boundary_link_count", static_cast<i64>(page->rivers->boundaryLinks.size())},
                    {"revision_fingerprint", static_cast<i64>(page->revisionFingerprint)}});
                for (const auto& node : page->rivers->nodes)
                {
                    nodes.emplace_back(rpc::Value::Object{
                        {"id", node.id.ToString()},
                        {"basin_id", node.basin.ToString()},
                        {"page_face", static_cast<i64>(tile.face)},
                        {"page_level", static_cast<i64>(tile.level)},
                        {"page_x", static_cast<i64>(tile.x)},
                        {"page_y", static_cast<i64>(tile.y)},
                        {"source_x", static_cast<i64>(node.sourceX)},
                        {"source_y", static_cast<i64>(node.sourceY)},
                        {"local_x_meters", node.channelOffsetMeters.x},
                        {"local_y_meters", node.channelOffsetMeters.y},
                        {"surface_elevation_meters", static_cast<f64>(node.surfaceHeightMeters)},
                        {"drainage_area_square_meters", node.drainageAreaSquareMeters},
                        {"discharge_cubic_meters_per_second", node.dischargeCubicMetersPerSecond},
                        {"channel_width_meters", static_cast<f64>(node.channelWidthMeters)},
                        {"channel_depth_meters", static_cast<f64>(node.channelDepthMeters)},
                        {"water_level_meters", static_cast<f64>(node.waterLevelMeters)},
                        {"slope", static_cast<f64>(node.slope)},
                        {"velocity_meters_per_second", static_cast<f64>(node.velocityMetersPerSecond)},
                        {"manning_roughness", static_cast<f64>(node.manningRoughness)},
                        {"cross_section_area_square_meters", node.crossSectionAreaSquareMeters},
                        {"suspended_sediment_kg", node.suspendedSedimentKg},
                        {"outlet", node.outlet},
                        {"exits_page", node.exitsPage}});
                }
                for (const auto& segment : page->rivers->segments)
                {
                    rpc::Value::Array routingPath;
                    routingPath.reserve(segment.routingPathMeters.size());
                    for (const auto& point : segment.routingPathMeters)
                        routingPath.emplace_back(rpc::Value::Array{point.x, point.y});
                    segments.emplace_back(rpc::Value::Object{
                        {"id", segment.id.ToString()},
                        {"basin_id", segment.basin.ToString()},
                        {"page_face", static_cast<i64>(tile.face)},
                        {"page_level", static_cast<i64>(tile.level)},
                        {"page_x", static_cast<i64>(tile.x)},
                        {"page_y", static_cast<i64>(tile.y)},
                        {"upstream_node_id", segment.upstreamNode < page->rivers->nodes.size()
                            ? page->rivers->nodes[segment.upstreamNode].id.ToString() : std::string{}},
                        {"downstream_node_id", segment.downstreamNode < page->rivers->nodes.size()
                            ? page->rivers->nodes[segment.downstreamNode].id.ToString() : std::string{}},
                        {"active", segment.active},
                        {"slope", static_cast<f64>(segment.slope)},
                        {"velocity_meters_per_second", static_cast<f64>(segment.velocityMetersPerSecond)},
                        {"manning_roughness", static_cast<f64>(segment.manningRoughness)},
                        {"suspended_sediment_kg", segment.suspendedSedimentKg},
                        {"routing_path_meters", std::move(routingPath)}});
                }
                for (const auto& link : page->rivers->boundaryLinks)
                {
                    ++boundaryLinkCount;
                    boundaryLinks.emplace_back(rpc::Value::Object{
                        {"upstream_node_id", link.upstreamNode.ToString()},
                        {"basin_id", link.basin.ToString()},
                        {"page_face", static_cast<i64>(tile.face)},
                        {"page_level", static_cast<i64>(tile.level)},
                        {"page_x", static_cast<i64>(tile.x)},
                        {"page_y", static_cast<i64>(tile.y)},
                        {"flow_dx", static_cast<i64>(link.flowDx)},
                        {"flow_dy", static_cast<i64>(link.flowDy)},
                        {"target_x", static_cast<i64>(link.targetX)},
                        {"target_y", static_cast<i64>(link.targetY)}});
                }
            }
            return rpc::Value(rpc::Value::Object{
                {"terrain", terrain->ToString()},
                {"viewport", viewport},
                {"observer_page", rpc::Value::Object{
                    {"face", static_cast<i64>(runtime->observerPhysicalPage.tile.face)},
                    {"level", static_cast<i64>(runtime->observerPhysicalPage.tile.level)},
                    {"x", static_cast<i64>(runtime->observerPhysicalPage.tile.x)},
                    {"y", static_cast<i64>(runtime->observerPhysicalPage.tile.y)}}},
                {"pending_page_count", static_cast<i64>(pendingPages)},
                {"boundary_link_count", static_cast<i64>(boundaryLinkCount)},
                {"pages", std::move(pages)},
                {"nodes", std::move(nodes)},
                {"segments", std::move(segments)},
                {"boundary_links", std::move(boundaryLinks)}});
        });

    dispatcher.Register(
        {
            .name = "terrain.lakes_nearby",
            .description = "Lists M09-derived lake basins and page-local spill diagnostics on built physical terrain pages near the active viewport observer.",
            .mutating = false
        },
        [&session](const rpc::Value& params)
        {
            const auto& values = RequireObject(params);
            const std::string viewport = values.contains("viewport")
                ? RequireString(values, "viewport")
                : "studio.primary";
            const auto terrain = scene::ObjectId::Parse(
                RequireString(values, "terrain"));
            if (!terrain.has_value())
                throw rpc::Error(-32602, "terrain must be a terrain object id.");

            const auto runtime = session.TerrainRuntime().Capture(viewport);
            if (!runtime.has_value())
                throw rpc::Error(1004, "No active terrain viewport runtime is available.");
            const auto body = session.World().Surfaces().BodyForTerrainObject(*terrain);
            if (!body.has_value() || *body != runtime->body)
                throw rpc::Error(-32602, "terrain does not match the active viewport body.");

            const auto addresses = world::TileNeighborhood(
                runtime->observerPhysicalPage.tile, 1U);
            rpc::Value::Array pages;
            rpc::Value::Array lakes;
            u64 pendingPages = 0U;
            for (const auto& tile : addresses)
            {
                const terrain::PhysicalTerrainPageAddress address{
                    .planet = runtime->planet.id,
                    .tile = tile};
                const auto page = session.TerrainPhysicalPages().Find(address);
                if (page == nullptr || page->lakes == nullptr)
                {
                    ++pendingPages;
                    continue;
                }
                pages.emplace_back(rpc::Value::Object{
                    {"face", static_cast<i64>(tile.face)},
                    {"level", static_cast<i64>(tile.level)},
                    {"x", static_cast<i64>(tile.x)},
                    {"y", static_cast<i64>(tile.y)},
                    {"basin_count", static_cast<i64>(page->lakes->basins.size())},
                    {"revision_fingerprint", static_cast<i64>(page->revisionFingerprint)}});
                for (const auto& basin : page->lakes->basins)
                {
                    const u32 resolution = page->lakes->resolution;
                    const i64 spillX = basin.spillCellIndex < resolution * resolution
                        ? static_cast<i64>(basin.spillCellIndex % resolution) : -1;
                    const i64 spillY = basin.spillCellIndex < resolution * resolution
                        ? static_cast<i64>(basin.spillCellIndex / resolution) : -1;
                    const i64 outletX = basin.outletCellIndex < resolution * resolution
                        ? static_cast<i64>(basin.outletCellIndex % resolution) : -1;
                    const i64 outletY = basin.outletCellIndex < resolution * resolution
                        ? static_cast<i64>(basin.outletCellIndex / resolution) : -1;
                    lakes.emplace_back(rpc::Value::Object{
                        {"id", std::to_string(basin.id)},
                        {"page_face", static_cast<i64>(tile.face)},
                        {"page_level", static_cast<i64>(tile.level)},
                        {"page_x", static_cast<i64>(tile.x)},
                        {"page_y", static_cast<i64>(tile.y)},
                        {"surface_elevation_meters", static_cast<f64>(basin.surfaceElevationMeters)},
                        {"maximum_depth_meters", static_cast<f64>(basin.maximumDepthMeters)},
                        {"area_square_meters", basin.areaSquareMeters},
                        {"cell_count", static_cast<i64>(basin.cellCount)},
                        {"spill_x", spillX},
                        {"spill_y", spillY},
                        {"outlet_x", outletX},
                        {"outlet_y", outletY},
                        {"spill_exits_page", basin.spillExitsPage},
                        {"downstream_river_node_id", basin.downstreamRiverNode.IsValid()
                            ? basin.downstreamRiverNode.ToString() : std::string{}},
                        {"downstream_river_cell_count", static_cast<i64>(basin.downstreamRiverCellCount)},
                        {"downstream_river_exits_page", basin.downstreamRiverExitsPage}});
                }
            }
            return rpc::Value(rpc::Value::Object{
                {"terrain", terrain->ToString()},
                {"viewport", viewport},
                {"pending_page_count", static_cast<i64>(pendingPages)},
                {"pages", std::move(pages)},
                {"lakes", std::move(lakes)}});
        });

    dispatcher.Register(
        {
            .name = "terrain.river_constraint_add",
            .description = "Creates a persisted basin-local M16 river constraint, selects it, and queues bounded terrain regeneration.",
            .mutating = true
        },
        [&session](const rpc::Value& params)
        {
            const auto& values = RequireObject(params);
            static constexpr std::array<std::string_view, 11> allowed{
                "terrain", "basin_id", "kind", "page_face", "page_level",
                "page_x", "page_y", "center_meters", "direction_meters",
                "radius_meters", "strength"};
            for (const auto& [key, value] : values)
            {
                static_cast<void>(value);
                if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
                    throw rpc::Error(-32602, "Unknown river constraint field: " + key + ".");
            }
            const auto terrain = scene::ObjectId::Parse(RequireString(values, "terrain"));
            const auto basin = terrain_erosion::RiverBasinId::Parse(RequireString(values, "basin_id"));
            if (!terrain || !basin)
                throw rpc::Error(-32602, "terrain and basin_id must be valid IDs.");
            const auto requireInt = [&values](const char* key) -> i64
            {
                const auto found = values.find(key);
                if (found == values.end() || !found->second.IsInteger())
                    throw rpc::Error(-32602, std::string(key) + " must be an integer.");
                return found->second.AsInteger();
            };
            const auto requireNumber = [&values](const char* key, const f64 fallback) -> f64
            {
                const auto found = values.find(key);
                if (found == values.end()) return fallback;
                if (!found->second.IsNumber())
                    throw rpc::Error(-32602, std::string(key) + " must be a number.");
                return found->second.AsNumber();
            };
            const auto readPair = [&values](const char* key, const math::Double2 fallback)
            {
                const auto found = values.find(key);
                if (found == values.end()) return fallback;
                if (!found->second.IsArray() || found->second.AsArray().size() != 2U ||
                    !found->second.AsArray()[0].IsNumber() ||
                    !found->second.AsArray()[1].IsNumber())
                    throw rpc::Error(-32602, std::string(key) + " must be [x, y].");
                return math::Double2{
                    found->second.AsArray()[0].AsNumber(),
                    found->second.AsArray()[1].AsNumber()};
            };
            const std::string kindName = RequireString(values, "kind");
            terrain_erosion::RiverConstraintKind kind{};
            if (kindName == "attract") kind = terrain_erosion::RiverConstraintKind::Attract;
            else if (kindName == "repel") kind = terrain_erosion::RiverConstraintKind::Repel;
            else if (kindName == "trajectory") kind = terrain_erosion::RiverConstraintKind::Trajectory;
            else throw rpc::Error(-32602, "kind must be attract, repel, or trajectory.");

            const i64 face = requireInt("page_face");
            const i64 level = requireInt("page_level");
            const i64 x = requireInt("page_x");
            const i64 y = requireInt("page_y");
            if (face < 0 || face > 5 || level < 0 || level > 30 || x < 0 || y < 0 ||
                static_cast<u64>(x) >= (u64{1} << static_cast<u32>(level)) ||
                static_cast<u64>(y) >= (u64{1} << static_cast<u32>(level)))
                throw rpc::Error(-32602, "The physical page address is invalid.");
            if (!values.contains("center_meters"))
                throw rpc::Error(-32602, "center_meters is required.");
            const math::Double2 center = readPair("center_meters", {});
            const math::Double2 direction = readPair("direction_meters", {1.0, 0.0});
            const f64 radius = requireNumber("radius_meters", 500.0);
            const f64 strength = requireNumber("strength", 1.0);
            try
            {
                const auto body = session.World().Surfaces().BodyForTerrainObject(*terrain);
                if (!body) throw rpc::Error(-32602, "Terrain object has no spherical body.");
                const auto planet = session.World().Surfaces().Registry().SphericalPlanetDefinition(*body);
                if (!planet) throw rpc::Error(-32602, "Terrain body has no spherical planet definition.");
                const terrain::PhysicalTerrainPageAddress page{
                    .planet = planet->id,
                    .tile = {
                        .face = static_cast<world::CubeFace>(face),
                        .level = static_cast<u8>(level),
                        .x = static_cast<u32>(x),
                        .y = static_cast<u32>(y)}};
                auto model = SurfaceModel(session);
                const auto object = model.AddRiverBasinConstraint(
                    *terrain, page, *basin, kind, center, direction, radius, strength);
                model.SelectObject(object);
                return rpc::Value(rpc::Value::Object{
                    {"terrain", terrain->ToString()},
                    {"constraint", object.ToString()},
                    {"basin_id", basin->ToString()},
                    {"kind", kindName},
                    {"rebuild_queued", true}});
            }
            catch (const rpc::Error&) { throw; }
            catch (const std::invalid_argument& exception) { throw rpc::Error(-32602, exception.what()); }
            catch (const std::exception& exception) { throw rpc::Error(1004, exception.what()); }
        });

    dispatcher.Register(
        {
            .name = "terrain.rivers_set",
            .description = "Updates supplied mountain-fed river recipe fields as one undoable edit, using the same SurfaceAuthoringModel operation as the Planet toolbar.",
            .mutating = true
        },
        [&session](const rpc::Value& params)
        {
            const auto& values = RequireObject(params);
            const auto terrain = scene::ObjectId::Parse(RequireString(values, "terrain"));
            if (!terrain.has_value()) throw rpc::Error(-32602, "terrain must be a terrain object id.");
            static constexpr std::array<std::string_view, 22> fields{
                "enabled", "minimum_drainage_area_square_meters",
                "maximum_node_spacing_meters",
                "minimum_discharge_cubic_meters_per_second",
                "reference_discharge_cubic_meters_per_second",
                "base_channel_width_meters", "minimum_channel_width_meters",
                "maximum_channel_width_meters", "width_discharge_exponent",
                "base_channel_depth_meters", "minimum_channel_depth_meters",
                "maximum_channel_depth_meters", "depth_discharge_exponent",
                "meanders", "meander_iterations", "meander_time_step",
                "curvature_migration_rate", "seed_migration_rate",
                "maximum_centerline_offset_widths", "cutoffs",
                "minimum_cutoff_path_nodes", "cutoff_distance_widths"};
            for (const auto& [key, value] : values)
            {
                static_cast<void>(value);
                if (key != "terrain" && std::find(fields.begin(), fields.end(), key) == fields.end())
                    throw rpc::Error(-32602, "Unknown river setting: " + key + ".");
            }
            try
            {
                auto model = SurfaceModel(session);
                auto settings = model.ProcessSettings(*terrain);
                const auto boolean = [&values](const char* key, bool& target)
                {
                    const auto found = values.find(key);
                    if (found == values.end()) return;
                    if (!found->second.IsBool()) throw rpc::Error(-32602, std::string(key) + " must be a boolean.");
                    target = found->second.AsBool();
                };
                const auto number = [&values](const char* key, f64& target)
                {
                    const auto found = values.find(key);
                    if (found == values.end()) return;
                    if (!found->second.IsNumber()) throw rpc::Error(-32602, std::string(key) + " must be a number.");
                    target = found->second.AsNumber();
                };
                boolean("enabled", settings.riversEnabled);
                boolean("meanders", settings.rivers.enableMeanders);
                boolean("cutoffs", settings.rivers.enableCutoffs);
                number("maximum_node_spacing_meters", settings.rivers.maximumNodeSpacingMeters);
                number("minimum_drainage_area_square_meters", settings.rivers.minimumDrainageAreaSquareMeters);
                number("minimum_discharge_cubic_meters_per_second", settings.rivers.minimumDischargeCubicMetersPerSecond);
                number("reference_discharge_cubic_meters_per_second", settings.rivers.referenceDischargeCubicMetersPerSecond);
                number("base_channel_width_meters", settings.rivers.baseChannelWidthMeters);
                number("minimum_channel_width_meters", settings.rivers.minimumChannelWidthMeters);
                number("maximum_channel_width_meters", settings.rivers.maximumChannelWidthMeters);
                number("width_discharge_exponent", settings.rivers.widthDischargeExponent);
                number("base_channel_depth_meters", settings.rivers.baseChannelDepthMeters);
                number("minimum_channel_depth_meters", settings.rivers.minimumChannelDepthMeters);
                number("maximum_channel_depth_meters", settings.rivers.maximumChannelDepthMeters);
                number("depth_discharge_exponent", settings.rivers.depthDischargeExponent);
                number("meander_time_step", settings.rivers.meanderTimeStep);
                number("curvature_migration_rate", settings.rivers.curvatureMigrationRate);
                number("seed_migration_rate", settings.rivers.deterministicSeedMigrationRate);
                number("maximum_centerline_offset_widths", settings.rivers.maximumCenterlineOffsetWidths);
                number("cutoff_distance_widths", settings.rivers.cutoffDistanceWidths);
                if (const auto found = values.find("meander_iterations"); found != values.end())
                {
                    if (!found->second.IsInteger() || found->second.AsInteger() < 1)
                        throw rpc::Error(-32602, "meander_iterations must be a positive integer.");
                    settings.rivers.meanderIterations = static_cast<u32>(found->second.AsInteger());
                }
                if (const auto found = values.find("minimum_cutoff_path_nodes"); found != values.end())
                {
                    if (!found->second.IsInteger() || found->second.AsInteger() < 2)
                        throw rpc::Error(-32602, "minimum_cutoff_path_nodes must be an integer of at least 2.");
                    settings.rivers.minimumCutoffPathNodes = static_cast<u32>(found->second.AsInteger());
                }
                model.SetProcessSettings(*terrain, settings);
                QueueGlobalProcessSettingsInvalidation(session, *terrain);
                return rpc::Value(rpc::Value::Object{
                    {"terrain", terrain->ToString()},
                    {"enabled", settings.riversEnabled},
                    {"maximum_node_spacing_meters", settings.rivers.maximumNodeSpacingMeters},
                    {"minimum_drainage_area_square_meters", settings.rivers.minimumDrainageAreaSquareMeters},
                    {"minimum_discharge_cubic_meters_per_second", settings.rivers.minimumDischargeCubicMetersPerSecond},
                    {"reference_discharge_cubic_meters_per_second", settings.rivers.referenceDischargeCubicMetersPerSecond},
                    {"base_channel_width_meters", settings.rivers.baseChannelWidthMeters},
                    {"minimum_channel_width_meters", settings.rivers.minimumChannelWidthMeters},
                    {"maximum_channel_width_meters", settings.rivers.maximumChannelWidthMeters},
                    {"width_discharge_exponent", settings.rivers.widthDischargeExponent},
                    {"base_channel_depth_meters", settings.rivers.baseChannelDepthMeters},
                    {"minimum_channel_depth_meters", settings.rivers.minimumChannelDepthMeters},
                    {"maximum_channel_depth_meters", settings.rivers.maximumChannelDepthMeters},
                    {"depth_discharge_exponent", settings.rivers.depthDischargeExponent},
                    {"meanders", settings.rivers.enableMeanders},
                    {"cutoffs", settings.rivers.enableCutoffs},
                    {"meander_iterations", static_cast<i64>(settings.rivers.meanderIterations)},
                    {"meander_time_step", settings.rivers.meanderTimeStep},
                    {"curvature_migration_rate", settings.rivers.curvatureMigrationRate},
                    {"seed_migration_rate", settings.rivers.deterministicSeedMigrationRate},
                    {"maximum_centerline_offset_widths", settings.rivers.maximumCenterlineOffsetWidths},
                    {"minimum_cutoff_path_nodes", static_cast<i64>(settings.rivers.minimumCutoffPathNodes)},
                    {"cutoff_distance_widths", settings.rivers.cutoffDistanceWidths}
                });
            }
            catch (const rpc::Error&) { throw; }
            catch (const std::invalid_argument& exception) { throw rpc::Error(-32602, exception.what()); }
            catch (const std::exception& exception) { throw rpc::Error(1004, exception.what()); }
        });

    dispatcher.Register(
        {
            .name = "terrain.cache_stats",
            .description =
                "Persistent GPU terrain cache statistics (hits, misses, generations, evictions, resident pages and bytes) for a terrain object, plus the stationary-camera cache counters of a viewport. After warmup with a stationary camera hits may grow but misses and generations must not.",
            .mutating = false
        },
        [&session](const rpc::Value& params)
        {
            const auto& values = RequireObject(params);

            const auto terrain =
                scene::ObjectId::Parse(
                    RequireString(values, "terrain"));

            if (!terrain.has_value())
            {
                throw rpc::Error(
                    -32602,
                    "terrain must be a terrain object id.");
            }

            std::string viewport = "studio.primary";

            if (const auto found = values.find("viewport");
                found != values.end())
            {
                if (!found->second.IsString() ||
                    found->second.AsString().empty())
                {
                    throw rpc::Error(
                        -32602,
                        "viewport must be a non-empty string.");
                }

                viewport = found->second.AsString();
            }

            const auto status =
                StudioTerrainStatusInspector::Capture(
                    session,
                    *terrain,
                    nullptr,
                    viewport);

            if (!status.has_value())
            {
                throw rpc::Error(
                    1004,
                    "No terrain services for that object (is a world open and the object a terrain surface?).");
            }

            const auto performance =
                session.TerrainPerformance().Capture(
                    session,
                    viewport);

            rpc::Value::Object stationary{
                {"frames",
                 static_cast<i64>(performance.stationaryFrames)},
                {"cache_hits",
                 static_cast<i64>(performance.stationaryCacheHits)},
                {"cache_misses",
                 static_cast<i64>(performance.stationaryCacheMisses)},
                {"hit_rate_percent",
                 performance.stationaryCacheHitRatePercent}
            };

            return rpc::Value(
                rpc::Value::Object{
                    {"terrain", terrain->ToString()},
                    {"viewport", status->selectedViewport},
                    {"cache", CacheStatsToRpc(status->cacheStats)},
                    {"stationary",
                     rpc::Value(std::move(stationary))},
                    {"physical_lod",
                     status->selectedPhysicalLod.has_value()
                         ? rpc::Value(
                               static_cast<i64>(
                                   *status->selectedPhysicalLod))
                         : rpc::Value{}},
                    {"semantic_revision",
                     static_cast<i64>(status->semanticRevision)},
                    {"surface_source_revision",
                     static_cast<i64>(status->surfaceSourceRevision)}
                });
        });
}
} // namespace orbit::studio_session
