#include <algorithm>
#include <orbit/studio_ui/StudioRenderViewRpc.hpp>

#include <cmath>
#include <optional>
#include <stdexcept>
#include <utility>
#include <string>

namespace orbit::studio_ui
{
namespace
{
using rpc::Value;

constexpr i64 kInvalid = 1070;
constexpr i64 kFailed = 1071;

[[nodiscard]] const Value::Object& RequireObject(
    const Value& params)
{
    if (!params.IsObject())
    {
        throw rpc::Error(-32602, "Params must be an object.");
    }

    return params.AsObject();
}

[[nodiscard]] std::string RequireString(
    const Value::Object& object,
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

[[nodiscard]] std::string ViewIdOrPrimary(const Value& params)
{
    if (params.IsObject())
    {
        const auto found = params.AsObject().find("id");
        if (found != params.AsObject().end() && found->second.IsString() &&
            !found->second.AsString().empty())
        {
            return found->second.AsString();
        }
    }
    return "studio.primary";
}

[[nodiscard]] Value Double3ToRpc(const math::Double3& value)
{
    return Value(Value::Array{value.x, value.y, value.z});
}

[[nodiscard]] math::Double3 Double3FromRpc(
    const Value::Object& object,
    const char* key)
{
    const auto found = object.find(key);
    if (found == object.end() || !found->second.IsArray() ||
        found->second.AsArray().size() != 3U)
    {
        throw rpc::Error(
            -32602,
            std::string(key) + " must be an array of three numbers.");
    }
    const auto& array = found->second.AsArray();
    for (const Value& element : array)
    {
        if (!element.IsNumber() || !std::isfinite(element.AsNumber()))
        {
            throw rpc::Error(
                -32602,
                std::string(key) + " must contain finite numbers.");
        }
    }
    return {
        array[0].AsNumber(),
        array[1].AsNumber(),
        array[2].AsNumber()};
}

[[nodiscard]] f64 NumberFromRpc(
    const Value::Object& object,
    const char* key)
{
    const auto found = object.find(key);
    if (found == object.end() || !found->second.IsNumber() ||
        !std::isfinite(found->second.AsNumber()))
    {
        throw rpc::Error(
            -32602, std::string(key) + " must be a finite number.");
    }
    return found->second.AsNumber();
}

[[nodiscard]] Value PoseToRpc(const std::string& id, const StudioViewPose& pose)
{
    return Value(Value::Object{
        {"id", id},
        {"target_object", pose.targetObject},
        {"terrain", pose.terrain},
        {"has_camera", pose.hasCamera},
        {"zoom", pose.zoom},
        {"observer", Double3ToRpc(pose.observerMeters)},
        {"east", Double3ToRpc(pose.surfaceFrame.east)},
        {"north", Double3ToRpc(pose.surfaceFrame.north)},
        {"up", Double3ToRpc(pose.surfaceFrame.up)},
        {"yaw", pose.yawRadians},
        {"pitch", pose.pitchRadians}});
}

[[nodiscard]] StudioViewPose PoseFromRpc(const Value::Object& object)
{
    StudioViewPose pose{};
    pose.targetObject = RequireString(object, "target_object");
    if (const auto zoom = object.find("zoom");
        zoom != object.end() && zoom->second.IsNumber() &&
        std::isfinite(zoom->second.AsNumber()))
    {
        pose.zoom = zoom->second.AsNumber();
    }
    const auto hasCamera = object.find("has_camera");
    pose.hasCamera = hasCamera == object.end() || !hasCamera->second.IsBool() ||
        hasCamera->second.AsBool();
    if (!pose.hasCamera)
    {
        return pose;
    }
    pose.observerMeters = Double3FromRpc(object, "observer");
    pose.surfaceFrame.east = Double3FromRpc(object, "east");
    pose.surfaceFrame.north = Double3FromRpc(object, "north");
    pose.surfaceFrame.up = Double3FromRpc(object, "up");
    pose.yawRadians = NumberFromRpc(object, "yaw");
    pose.pitchRadians = NumberFromRpc(object, "pitch");
    const auto terrain = object.find("terrain");
    pose.terrain = terrain != object.end() && terrain->second.IsBool() &&
        terrain->second.AsBool();
    return pose;
}

[[nodiscard]] lighting::SurfaceDebugMode ParseSurfaceDebugMode(
    const std::string_view text)
{
    if (text == "lit")
    {
        return lighting::SurfaceDebugMode::Lit;
    }
    if (text == "base_color_roughness")
    {
        return lighting::SurfaceDebugMode::BaseColorRoughness;
    }
    if (text == "normal_metallic")
    {
        return lighting::SurfaceDebugMode::NormalMetallic;
    }
    if (text == "emission_metadata")
    {
        return lighting::SurfaceDebugMode::EmissionMetadata;
    }

    throw rpc::Error(
        -32602,
        "surface_debug_mode must be lit, base_color_roughness, "
        "normal_metallic, or emission_metadata.");
}

[[nodiscard]] const char* SurfaceDebugModeName(
    const lighting::SurfaceDebugMode mode) noexcept
{
    switch (mode)
    {
    case lighting::SurfaceDebugMode::Lit:
        return "lit";
    case lighting::SurfaceDebugMode::BaseColorRoughness:
        return "base_color_roughness";
    case lighting::SurfaceDebugMode::NormalMetallic:
        return "normal_metallic";
    case lighting::SurfaceDebugMode::EmissionMetadata:
        return "emission_metadata";
    }

    return "lit";
}

[[nodiscard]] Value ToRpc(
    const std::string& id,
    const lighting::SurfaceDebugMode mode)
{
    return Value(
        Value::Object{
            {"id", id},
            {"surface_debug_mode", SurfaceDebugModeName(mode)}
        });
}

struct OverlayFlag
{
    const char* name;
    bool StudioTerrainDiagnosticOverlayOptions::* member;
};

constexpr OverlayFlag kOverlayFlags[] = {
    {"dirty_page_bounds", &StudioTerrainDiagnosticOverlayOptions::dirtyPageBounds},
    {"build_states", &StudioTerrainDiagnosticOverlayOptions::buildStates},
    {"physical_lod", &StudioTerrainDiagnosticOverlayOptions::physicalLod},
    {"clipmap_rings", &StudioTerrainDiagnosticOverlayOptions::clipmapRings},
    {"clipmap_levels", &StudioTerrainDiagnosticOverlayOptions::clipmapLevels},
    {"clipmap_sample_health", &StudioTerrainDiagnosticOverlayOptions::clipmapSampleHealth},
    {"clipmap_hole_view", &StudioTerrainDiagnosticOverlayOptions::clipmapHoleView},
    {"clipmap_projection_view", &StudioTerrainDiagnosticOverlayOptions::clipmapProjectionView},
    {"clipmap_shading_view", &StudioTerrainDiagnosticOverlayOptions::clipmapShadingView},
    {"clipmap_wireframe", &StudioTerrainDiagnosticOverlayOptions::clipmapWireframe},
    {"clipmap_freeze", &StudioTerrainDiagnosticOverlayOptions::clipmapFreeze},
    {"cache_status", &StudioTerrainDiagnosticOverlayOptions::cacheStatus},
    {"authored_constraints", &StudioTerrainDiagnosticOverlayOptions::authoredConstraints},
    {"biome_weights", &StudioTerrainDiagnosticOverlayOptions::biomeWeights},
    {"process_masks", &StudioTerrainDiagnosticOverlayOptions::processMasks},
    {"drainage_vectors", &StudioTerrainDiagnosticOverlayOptions::drainageVectors}};

[[nodiscard]] Value OverlaysToRpc(
    const std::string& id,
    const StudioTerrainDiagnosticOverlayOptions& options)
{
    Value::Object result{{"id", id}};
    for (const auto& flag : kOverlayFlags)
    {
        result.emplace(flag.name, options.*flag.member);
    }
    return Value(std::move(result));
}

// The band edges up to the first 0 (the list's end).
[[nodiscard]] Value BandEdgesToRpc(const StudioTerrainLayerOptions& layers)
{
    Value::Array edges;
    for (const f32 edge : layers.clipmapBandEdgesMeters)
    {
        if (edge <= 0.0F)
        {
            break;
        }
        edges.emplace_back(static_cast<f64>(edge));
    }
    return Value(std::move(edges));
}

[[nodiscard]] Value CloudLabToRpc(const StudioCloudLab& lab)
{
    return Value(Value::Object{
        {"enabled", lab.enabled},
        {"type", static_cast<f64>(lab.type)},
        {"coverage", static_cast<f64>(lab.coverage)},
        {"cirrus", static_cast<f64>(lab.cirrus)},
        {"precipitation", static_cast<f64>(lab.precipitation)},
        {"radius_meters", static_cast<f64>(lab.radiusMeters)},
        {"height_scale", static_cast<f64>(lab.heightScale)},
        {"distance_meters", static_cast<f64>(lab.distanceMeters)},
        {"sun_override", lab.overrideSun},
        {"sun_elevation_degrees", static_cast<f64>(lab.sunElevationDegrees)},
        {"sun_azimuth_degrees", static_cast<f64>(lab.sunAzimuthDegrees)},
        {"maturity", static_cast<f64>(lab.maturity)},
        {"organisation", static_cast<f64>(lab.organisation)},
        {"density", static_cast<f64>(lab.density)},
        {"cirrus_sheet", static_cast<f64>(lab.cirrusSheet)},
        {"seed", static_cast<i64>(lab.seed)},
        {"place_serial", static_cast<i64>(lab.placeSerial)}});
}

[[nodiscard]] Value LayersToRpc(
    const std::string& id,
    const StudioTerrainLayerOptions& layers)
{
    return Value(Value::Object{
        {"id", id},
        {"production_surface", layers.productionSurface},
        {"full_clipmap", layers.fullClipmap},
        {"macro_globe", layers.macroGlobe},
        {"ocean", layers.ocean},
        {"surface_effects", layers.surfaceEffects},
        {"lod_bias_stops", static_cast<f64>(layers.lodBiasStops)},
        {"dynamic_clipmaps", layers.dynamicClipmaps},
        {"clipmap_pixels_per_vertex",
         static_cast<f64>(layers.clipmapPixelsPerVertex)},
        {"clipmap_fade_seconds",
         static_cast<f64>(layers.clipmapFadeSeconds)},
        {"experimental_distance_bands", layers.experimentalDistanceBands},
        {"clipmap_band_scale", static_cast<f64>(layers.clipmapBandScale)},
        {"clipmap_partial_updates", layers.clipmapPartialUpdates},
        {"physical_pages", layers.physicalPages},
        {"clouds", layers.clouds},
        {"cloud_resolution_scale", static_cast<f64>(layers.cloudResolutionScale)},
        {"cloud_temporal", layers.cloudTemporal},
        {"cloud_godray_strength", static_cast<f64>(layers.cloudGodrayStrength)},
        {"cloud_light_volume", layers.cloudLightVolume},
        {"bypass_cloud_shadow", layers.bypassCloudShadow},
        {"bypass_indirect_lighting", layers.bypassIndirectLighting},
        {"bypass_near_field_water", layers.bypassNearFieldWater},
        {"bypass_atmosphere", layers.bypassAtmosphere},
        {"bypass_hybrid_reflections", layers.bypassHybridReflections},
        {"bypass_radiance_cache", layers.bypassRadianceCache},
        {"indirect_coverage_view", layers.indirectCoverageView},
        {"cloud_volume_debug_altitude", static_cast<f64>(layers.cloudVolumeDebugAltitude)},
        {"cloud_lab", CloudLabToRpc(layers.cloudLab)},
        {"clipmap_band_edges_meters", BandEdgesToRpc(layers)}});
}

[[nodiscard]] Value Vec3ToRpc(const math::Double3& value)
{
    return Value(Value::Array{value.x, value.y, value.z});
}

[[nodiscard]] Value PointToRpc(const StudioTerrainPointReport& point)
{
    Value::Object biomes;
    biomes.emplace("ocean", static_cast<f64>(point.biomes.ocean));
    biomes.emplace("desert", static_cast<f64>(point.biomes.desert));
    biomes.emplace("grassland", static_cast<f64>(point.biomes.grassland));
    biomes.emplace(
        "temperate_forest",
        static_cast<f64>(point.biomes.temperateForest));
    biomes.emplace(
        "boreal_forest",
        static_cast<f64>(point.biomes.borealForest));
    biomes.emplace("tundra", static_cast<f64>(point.biomes.tundra));
    biomes.emplace("alpine", static_cast<f64>(point.biomes.alpine));
    biomes.emplace("wetland", static_cast<f64>(point.biomes.wetland));

    return Value(Value::Object{
        {"unit_direction", Vec3ToRpc(point.unitDirection)},
        {"latitude_degrees", point.latitudeDegrees},
        {"longitude_degrees", point.longitudeDegrees},
        {"footprint_meters", point.footprintMeters},
        {"terrain_elevation_meters", point.terrainElevationMeters},
        {"coarse_elevation_meters", point.coarseElevationMeters},
        {"detail_delta_meters", point.detailDeltaMeters},
        {"standing_water_depth_meters", point.standingWaterDepthMeters},
        {"water_surface_elevation_meters", point.waterSurfaceElevationMeters},
        {"underwater", point.underwater},
        {"ground_radius_from_core_meters", point.groundRadiusFromCoreMeters},
        {"rendered_surface_radius_from_core_meters",
         point.renderedSurfaceRadiusFromCoreMeters},
        {"slope_degrees", point.slopeDegrees},
        {"downhill_bearing_degrees", point.downhillBearingDegrees},
        {"climate", Value::Object{
            {"temperature_c", static_cast<f64>(point.climate.temperatureC)},
            {"humidity", static_cast<f64>(point.climate.humidity)},
            {"precipitation", static_cast<f64>(point.climate.precipitation)},
            {"continentality",
             static_cast<f64>(point.climate.continentality)}}},
        {"biome_weights", Value(std::move(biomes))},
        {"dominant_biome", point.dominantBiome}});
}

[[nodiscard]] Value TextReportToRpc(
    const StudioViewportTextReport& report,
    const bool hud)
{
    Value::Object result{
        {"id", report.viewId},
        {"hud", hud},
        {"view_mode", report.viewMode},
        {"navigation", report.navigation},
        {"width", static_cast<i64>(report.width)},
        {"height", static_cast<i64>(report.height)},
        {"has_terrain", report.hasTerrain},
        {"camera", Value::Object{
            {"position_meters", Vec3ToRpc(report.cameraPosition)},
            {"forward", Vec3ToRpc(report.forward)},
            {"up", Vec3ToRpc(report.up)},
            {"heading_degrees", report.cameraHeadingDegrees},
            {"pitch_degrees", report.cameraPitchDegrees},
            {"vertical_fov_degrees", report.verticalFovDegrees},
            {"near_plane_meters", report.nearPlaneMeters},
            {"far_plane_meters", report.farPlaneMeters}}},
        {"distance_from_core_meters", report.distanceFromCoreMeters},
        {"layers", LayersToRpc(report.viewId, report.layers)},
        {"text", FormatStudioViewportTextReport(report)}};

    if (report.hasTerrain)
    {
        result.emplace("planet_radius_meters", report.planetRadiusMeters);
        result.emplace(
            "height_above_datum_meters",
            report.heightAboveDatumMeters);
        result.emplace(
            "height_above_terrain_meters",
            report.heightAboveTerrainMeters.has_value()
                ? Value(*report.heightAboveTerrainMeters)
                : Value(nullptr));
        result.emplace(
            "height_above_water_surface_meters",
            report.heightAboveWaterSurfaceMeters.has_value()
                ? Value(*report.heightAboveWaterSurfaceMeters)
                : Value(nullptr));
        result.emplace("terrain_runtime", Value::Object{
            {"physical_page_level", static_cast<i64>(report.physicalPageLevel)},
            {"adaptive_coverage_tier",
             static_cast<i64>(report.adaptiveCoverageTier)},
            {"terrain_source_revision",
             static_cast<i64>(report.terrainSourceRevision)},
            {"world_generation", static_cast<i64>(report.worldGeneration)},
            {"runtime_generation",
             static_cast<i64>(report.runtimeGeneration)}});
    }

    if (report.cpuTerrain.has_value())
    {
        const auto& cpu = *report.cpuTerrain;
        Value::Object pages;
        if (cpu.hasPages)
        {
            pages = Value::Object{
                {"state", cpu.pageState},
                {"pages", static_cast<i64>(cpu.pages)},
                {"dirty", static_cast<i64>(cpu.dirtyPages)},
                {"queued", static_cast<i64>(cpu.queuedPages)},
                {"building", static_cast<i64>(cpu.buildingPages)},
                {"uploading", static_cast<i64>(cpu.uploadingPages)},
                {"ready", static_cast<i64>(cpu.readyPages)},
                {"failed", static_cast<i64>(cpu.failedPages)},
                {"stale", static_cast<i64>(cpu.stalePages)},
                {"completed_products", static_cast<i64>(cpu.completedProducts)},
                {"total_products", static_cast<i64>(cpu.totalProducts)}};
        }
        result.emplace("cpu_terrain", Value::Object{
            {"page_pool", Value::Object{
                {"workers", static_cast<i64>(cpu.poolWorkers)},
                {"running", static_cast<i64>(cpu.poolRunningJobs)},
                {"queued", static_cast<i64>(cpu.poolQueuedJobs)},
                {"outstanding", static_cast<i64>(cpu.poolOutstandingJobs)}}},
            {"page_rebuild", cpu.hasPages ? Value(std::move(pages)) : Value(nullptr)},
            {"orbital_patches", Value::Object{
                {"building", static_cast<i64>(cpu.globePatchesPending)},
                {"resident", static_cast<i64>(cpu.globePatchesResident)}}}});
    }

    if (report.clipmapPlan.has_value())
    {
        const auto& plan = *report.clipmapPlan;
        // One entry per drawn level: its index, sample spacing and half extent
        // (and, for the experimental banded clipmap, its distance band).
        Value::Array activeLevels;
        for (const StudioClipmapLevelStats& level : plan.levels)
        {
            Value::Object entry{
                {"level", static_cast<i64>(level.level)},
                {"spacing_meters", level.spacingMeters},
                {"half_extent_meters", level.halfExtentMeters},
                {"grid_resolution", static_cast<i64>(level.gridResolution)},
                {"drawn_vertices", static_cast<i64>(level.drawnVertices)},
                {"expected_vertices",
                 static_cast<i64>(
                     6ULL * (static_cast<u64>(level.gridResolution) - 1ULL) *
                     (static_cast<u64>(level.gridResolution) - 1ULL))},
                {"fully_drawn",
                 level.drawnVertices ==
                     6ULL * (static_cast<u64>(level.gridResolution) - 1ULL) *
                         (static_cast<u64>(level.gridResolution) - 1ULL)}};
            if (plan.banded)
            {
                entry.emplace("band_inner_meters", level.bandInnerMeters);
                entry.emplace("band_outer_meters", level.bandOuterMeters);
            }
            activeLevels.emplace_back(std::move(entry));
        }
        result.emplace("clipmap_plan", Value::Object{
            {"levels", Value(std::move(activeLevels))},
            {"dynamic", plan.dynamic},
            {"frozen", plan.frozen},
            {"wireframe", plan.wireframe},
            {"banded", plan.banded},
            {"ladder_levels", static_cast<i64>(plan.ladderLevels)},
            {"first_level", static_cast<i64>(plan.firstLevel)},
            {"last_level", static_cast<i64>(plan.lastLevel)},
            {"active_levels",
             static_cast<i64>(plan.lastLevel >= plan.firstLevel
                                  ? plan.lastLevel - plan.firstLevel + 1U
                                  : 0U)},
            {"finest_spacing_meters", plan.finestSpacingMeters},
            {"coarsest_half_extent_meters", plan.coarsestHalfExtentMeters},
            {"nearest_ground_meters", plan.nearestGroundMeters},
            {"visible_arc_meters", plan.visibleArcMeters},
            {"required_spacing_meters", plan.requiredSpacingMeters},
            {"ground_elevation_meters", plan.groundElevationMeters},
            {"plan_changes", static_cast<i64>(plan.planChanges)},
            {"generated_samples", static_cast<i64>(plan.generatedSamples)},
            {"rendered_ground_elevation_meters",
             plan.renderedGroundValid
                 ? Value(plan.renderedGroundElevationMeters)
                 : Value(nullptr)},
            {"rendered_ground_cpu_elevation_meters",
             plan.renderedGroundValid
                 ? Value(plan.renderedGroundCpuElevationMeters)
                 : Value(nullptr)},
            {"rendered_ground_footprint_meters",
             plan.renderedGroundValid
                 ? Value(plan.renderedGroundFootprintMeters)
                 : Value(nullptr)}});
    }

    if (report.clouds.has_value())
    {
        const auto& clouds = *report.clouds;
        result.emplace("clouds", Value::Object{
            {"layer_count", static_cast<i64>(clouds.layerCount)},
            {"mean_coverage", clouds.meanCoverage},
            {"mean_optical_depth", clouds.meanOpticalDepth},
            {"time_bucket", clouds.timeBucket},
            {"fingerprint", static_cast<i64>(clouds.fingerprint)},
            {"gpu_resident", clouds.gpuResident},
            // The cloud field is composited into the orbital globe's appearance;
            // the full clipmap renderer does not draw the globe.
            {"composited_into_globe", !report.layers.fullClipmap}});
    }

    if (report.nadir.has_value())
    {
        result.emplace("below_camera", PointToRpc(*report.nadir));
    }

    if (report.cursor.has_value())
    {
        Value::Object cursor{
            {"hit_distance_meters", report.cursor->hitDistanceMeters},
            {"pick_physical_elevation_meters",
             report.cursor->pickPhysicalElevationMeters},
            {"pick_rendered_elevation_meters",
             report.cursor->pickRenderedElevationMeters},
            {"physical_page", report.cursor->physicalPage},
            {"point", PointToRpc(report.cursor->point)}};
        if (report.cursor->physicalLod.has_value())
        {
            cursor.emplace(
                "physical_lod",
                static_cast<i64>(*report.cursor->physicalLod));
        }
        result.emplace("under_cursor", Value(std::move(cursor)));
    }

    return Value(std::move(result));
}
} // namespace

void RegisterStudioRenderViewRpc(
    rpc::Dispatcher& dispatcher,
    StudioRenderViewSet& views)
{
    dispatcher.Register(
        {
            .name = "view.surface_debug_get",
            .description =
                "Returns the GBuffer debug-view channel a Studio "
                "RenderView is currently showing "
                "(lit | base_color_roughness | normal_metallic | "
                "emission_metadata).",
            .mutating = false
        },
        [&views](const Value& params)
        {
            const auto& values = RequireObject(params);
            const std::string id = RequireString(values, "id");

            try
            {
                return ToRpc(id, views.SurfaceDebugMode(id));
            }
            catch (const rpc::Error&)
            {
                throw;
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(kInvalid, exception.what());
            }
        });

    dispatcher.Register(
        {
            .name = "view.surface_debug_set",
            .description =
                "Switches a Studio RenderView's GBuffer debug-view channel "
                "(lit | base_color_roughness | normal_metallic | "
                "emission_metadata). The same operation the Debug tab and "
                "each Viewport panel's 'Surface View' buttons drive.",
            .mutating = true
        },
        [&views](const Value& params)
        {
            const auto& values = RequireObject(params);
            const std::string id = RequireString(values, "id");
            const auto mode =
                ParseSurfaceDebugMode(
                    RequireString(values, "surface_debug_mode"));

            try
            {
                views.SetSurfaceDebugMode(id, mode);
                return ToRpc(id, views.SurfaceDebugMode(id));
            }
            catch (const rpc::Error&)
            {
                throw;
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(kFailed, exception.what());
            }
        });

    dispatcher.Register(
        {
            .name = "view.terrain_overlays_get",
            .description =
                "Returns which terrain diagnostic overlays a Studio "
                "RenderView draws (dirty_page_bounds, build_states, "
                "physical_lod, clipmap_rings (active levels only), clipmap_levels (tints the terrain by level), "
                "clipmap_wireframe, clipmap_freeze, cache_status, "
                "authored_constraints, biome_weights, process_masks, "
                "drainage_vectors).",
            .mutating = false
        },
        [&views](const Value& params)
        {
            const auto& values = RequireObject(params);
            const std::string id = RequireString(values, "id");

            try
            {
                return OverlaysToRpc(id, views.TerrainDiagnosticOverlays(id));
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(kInvalid, exception.what());
            }
        });

    dispatcher.Register(
        {
            .name = "view.terrain_overlays_set",
            .description =
                "Turns terrain diagnostic overlays on or off for a Studio "
                "RenderView. Pass any of dirty_page_bounds, build_states, "
                "physical_lod, clipmap_rings (active levels only), clipmap_levels (tints the terrain by level), "
                "clipmap_wireframe (terrain as a wireframe), clipmap_freeze (the clipmap stops following the "
                "camera, which can fly away and look at the rings from outside), cache_status, "
                "authored_constraints, biome_weights, process_masks, "
                "drainage_vectors as booleans; omitted flags keep their "
                "value. The same toggles as the viewport's Diagnostics "
                "properties.",
            .mutating = true
        },
        [&views](const Value& params)
        {
            const auto& values = RequireObject(params);
            const std::string id = RequireString(values, "id");

            try
            {
                auto options = views.TerrainDiagnosticOverlays(id);
                for (const auto& flag : kOverlayFlags)
                {
                    const auto found = values.find(flag.name);
                    if (found == values.end())
                    {
                        continue;
                    }
                    if (!found->second.IsBool())
                    {
                        throw rpc::Error(
                            kInvalid,
                            std::string(flag.name) + " must be a boolean.");
                    }
                    options.*flag.member = found->second.AsBool();
                }
                views.SetTerrainDiagnosticOverlays(id, options);
                return OverlaysToRpc(id, views.TerrainDiagnosticOverlays(id));
            }
            catch (const rpc::Error&)
            {
                throw;
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(kFailed, exception.what());
            }
        });

    dispatcher.Register(
        {
            .name = "view.terrain_layers_get",
            .description =
                "Returns which terrain layers a Studio RenderView draws "
                "(production_surface = near-field clipmap terrain, "
                "full_clipmap = the clipmap draws from ground to orbit and "
                "replaces the orbital globe, "
                "macro_globe = orbital displaced-globe patches, ocean, "
                "surface_effects), its lod_bias_stops, and the dynamic "
                "clipmap planner (dynamic_clipmaps, "
                "clipmap_pixels_per_vertex, clipmap_fade_seconds, "
                "experimental_distance_bands, clipmap_band_edges_meters, "
                "clipmap_band_scale, clipmap_partial_updates, physical_pages, clouds).",
            .mutating = false
        },
        [&views](const Value& params)
        {
            const auto& values = RequireObject(params);
            const std::string id = RequireString(values, "id");

            try
            {
                return LayersToRpc(id, views.TerrainLayers(id));
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(kInvalid, exception.what());
            }
        });

    dispatcher.Register(
        {
            .name = "view.terrain_layers_set",
            .description =
                "Chooses which terrain layers a Studio RenderView draws and "
                "its LOD bias. Pass any of production_surface, macro_globe, "
                "ocean, surface_effects (booleans) and lod_bias_stops "
                "(number, clamped to [-4, 4]; +1 keeps richer representations "
                "longer and doubles orbital patch resolution, -1 the "
                "opposite). dynamic_clipmaps (boolean) draws and generates "
                "only the clipmap levels the camera can use; "
                "clipmap_pixels_per_vertex (number, [0.25, 32], default 3) "
                "is the target sample spacing in pixels at the nearest "
                "ground, lower keeps finer levels longer; "
                "clipmap_fade_seconds (number, [0, 5], default 0.4) is how "
                "long a level takes to dissolve in or out when the plan adds "
                "or drops it (0 = instant, they pop). "
                "experimental_distance_bands (boolean, default false) draws "
                "clipmap level k only where the camera's distance to the "
                "terrain lies between clipmap_band_edges_meters[k-1] and "
                "[k] (array of up to 16 increasing metres, default "
                "[100, 500, 2000, 10000, 40000, ...]; the last is the farthest "
                "distance drawn), cross-fading neighbours so the rings resize "
                "continuously; clipmap_band_scale (number, [0.1, 10], default "
                "1) multiplies every edge (rebuilds the renderer, applied in "
                "1/8-octave steps); clipmap_partial_updates (boolean, default "
                "true) makes banded levels refresh only the strip that "
                "scrolled into view (false regenerates a whole level on every "
                "scroll, for comparison); physical_pages (boolean, default "
                "true) composites the derived physical pages (the cache "
                "status bounds) into the clipmap, false draws the plain "
                "generated terrain; clouds (boolean, default true) ray-marches "
                "the body's cloud layer in the viewport; cloud_lab (object) "
                "replaces the weather with one isolated cloud of a chosen type "
                "for judging shape and self-shadowing: enabled, type (0.05 "
                "stratus .. 1.0 cumulonimbus), coverage, cirrus, "
                "precipitation, radius_meters, height_scale (exaggerates "
                "vertical development), distance_meters, sun_override with "
                "sun_elevation_degrees / sun_azimuth_degrees, maturity (life cycle "
                "0 towering .. 0.3 growing .. 0.6 mature anvil .. 0.9 "
                "dissipating), organisation (0 single cell, 0.5 multicell, 1 "
                "organised), density (0.2-6) and seed, and place "
                "(true places the cloud ahead of the camera). Omitted fields "
                "keep their value. The same "
                "controls as the viewport Diagnostics 'Terrain layers' "
                "section. Transient view state.",
            .mutating = true
        },
        [&views](const Value& params)
        {
            const auto& values = RequireObject(params);
            const std::string id = RequireString(values, "id");

            try
            {
                auto layers = views.TerrainLayers(id);

                const auto applyFlag =
                    [&values](const char* name, bool& target)
                {
                    const auto found = values.find(name);
                    if (found == values.end())
                    {
                        return;
                    }
                    if (!found->second.IsBool())
                    {
                        throw rpc::Error(
                            kInvalid,
                            std::string(name) + " must be a boolean.");
                    }
                    target = found->second.AsBool();
                };
                applyFlag("production_surface", layers.productionSurface);
                applyFlag("full_clipmap", layers.fullClipmap);
                applyFlag("macro_globe", layers.macroGlobe);
                applyFlag("ocean", layers.ocean);
                applyFlag("surface_effects", layers.surfaceEffects);
                applyFlag("dynamic_clipmaps", layers.dynamicClipmaps);

                if (const auto pixels = values.find("clipmap_pixels_per_vertex");
                    pixels != values.end())
                {
                    if (!pixels->second.IsNumber())
                    {
                        throw rpc::Error(
                            kInvalid,
                            "clipmap_pixels_per_vertex must be a number.");
                    }
                    layers.clipmapPixelsPerVertex =
                        static_cast<f32>(pixels->second.AsNumber());
                }

                applyFlag(
                    "experimental_distance_bands",
                    layers.experimentalDistanceBands);
                applyFlag(
                    "clipmap_partial_updates",
                    layers.clipmapPartialUpdates);
                applyFlag("physical_pages", layers.physicalPages);
                applyFlag("clouds", layers.clouds);
                applyFlag("cloud_temporal", layers.cloudTemporal);
                applyFlag("cloud_light_volume", layers.cloudLightVolume);
                applyFlag("bypass_cloud_shadow", layers.bypassCloudShadow);
                applyFlag("bypass_indirect_lighting", layers.bypassIndirectLighting);
                applyFlag("bypass_near_field_water", layers.bypassNearFieldWater);
                applyFlag("bypass_atmosphere", layers.bypassAtmosphere);
                applyFlag("bypass_hybrid_reflections", layers.bypassHybridReflections);
                applyFlag("bypass_radiance_cache", layers.bypassRadianceCache);
                applyFlag("indirect_coverage_view", layers.indirectCoverageView);
                if (const auto debugAltitude = values.find("cloud_volume_debug_altitude");
                    debugAltitude != values.end())
                {
                    if (!debugAltitude->second.IsNumber())
                    {
                        throw rpc::Error(
                            kInvalid, "cloud_volume_debug_altitude must be a number.");
                    }
                    layers.cloudVolumeDebugAltitude = std::clamp(
                        static_cast<f32>(debugAltitude->second.AsNumber()), -1.0F, 40000.0F);
                }
                if (const auto scale = values.find("cloud_resolution_scale");
                    scale != values.end())
                {
                    if (!scale->second.IsNumber())
                    {
                        throw rpc::Error(
                            kInvalid, "cloud_resolution_scale must be a number.");
                    }
                    layers.cloudResolutionScale = std::clamp(
                        static_cast<f32>(scale->second.AsNumber()), 0.25F, 1.0F);
                }
                if (const auto strength = values.find("cloud_godray_strength");
                    strength != values.end())
                {
                    if (!strength->second.IsNumber())
                    {
                        throw rpc::Error(
                            kInvalid, "cloud_godray_strength must be a number.");
                    }
                    layers.cloudGodrayStrength = std::clamp(
                        static_cast<f32>(strength->second.AsNumber()), 0.0F, 2.0F);
                }

                if (const auto labFound = values.find("cloud_lab");
                    labFound != values.end())
                {
                    if (!labFound->second.IsObject())
                    {
                        throw rpc::Error(kInvalid, "cloud_lab must be an object.");
                    }
                    const auto& labValues = labFound->second.AsObject();
                    auto& lab = layers.cloudLab;
                    const auto labFlag =
                        [&labValues](const char* name, bool& target)
                    {
                        const auto found = labValues.find(name);
                        if (found == labValues.end())
                        {
                            return;
                        }
                        if (!found->second.IsBool())
                        {
                            throw rpc::Error(
                                kInvalid,
                                std::string("cloud_lab.") + name + " must be a boolean.");
                        }
                        target = found->second.AsBool();
                    };
                    const auto labNumber =
                        [&labValues](const char* name, f32& target, const f32 low, const f32 high)
                    {
                        const auto found = labValues.find(name);
                        if (found == labValues.end())
                        {
                            return;
                        }
                        if (!found->second.IsNumber())
                        {
                            throw rpc::Error(
                                kInvalid,
                                std::string("cloud_lab.") + name + " must be a number.");
                        }
                        target = std::clamp(
                            static_cast<f32>(found->second.AsNumber()), low, high);
                    };
                    labFlag("enabled", lab.enabled);
                    labNumber("type", lab.type, 0.0F, 1.0F);
                    labNumber("coverage", lab.coverage, 0.0F, 1.0F);
                    labNumber("cirrus", lab.cirrus, 0.0F, 1.0F);
                    labNumber("precipitation", lab.precipitation, 0.0F, 1.0F);
                    labNumber("radius_meters", lab.radiusMeters, 200.0F, 200000.0F);
                    labNumber("height_scale", lab.heightScale, 0.25F, 4.0F);
                    labNumber("distance_meters", lab.distanceMeters, 500.0F, 500000.0F);
                    labFlag("sun_override", lab.overrideSun);
                    labNumber("sun_elevation_degrees", lab.sunElevationDegrees, -10.0F, 90.0F);
                    labNumber("sun_azimuth_degrees", lab.sunAzimuthDegrees, 0.0F, 360.0F);
                    labNumber("maturity", lab.maturity, 0.0F, 1.0F);
                    labNumber("organisation", lab.organisation, 0.0F, 1.0F);
                    labNumber("density", lab.density, 0.2F, 6.0F);
                    labNumber("cirrus_sheet", lab.cirrusSheet, 0.0F, 1.0F);
                    f32 seedValue = static_cast<f32>(lab.seed);
                    labNumber("seed", seedValue, 0.0F, 100000.0F);
                    lab.seed = static_cast<u32>(seedValue);
                    bool place = false;
                    labFlag("place", place);
                    if (place)
                    {
                        ++lab.placeSerial;
                    }
                }

                if (const auto edges = values.find("clipmap_band_edges_meters");
                    edges != values.end())
                {
                    if (!edges->second.IsArray() ||
                        edges->second.AsArray().size() >
                            layers.clipmapBandEdgesMeters.size())
                    {
                        throw rpc::Error(
                            kInvalid,
                            "clipmap_band_edges_meters must be an array of at most 16 numbers.");
                    }
                    layers.clipmapBandEdgesMeters.fill(0.0F);
                    std::size_t slot = 0U;
                    for (const auto& entry : edges->second.AsArray())
                    {
                        if (!entry.IsNumber())
                        {
                            throw rpc::Error(
                                kInvalid,
                                "clipmap_band_edges_meters must contain numbers.");
                        }
                        layers.clipmapBandEdgesMeters[slot++] =
                            static_cast<f32>(entry.AsNumber());
                    }
                }

                if (const auto scale = values.find("clipmap_band_scale");
                    scale != values.end())
                {
                    if (!scale->second.IsNumber())
                    {
                        throw rpc::Error(
                            kInvalid,
                            "clipmap_band_scale must be a number.");
                    }
                    layers.clipmapBandScale =
                        static_cast<f32>(scale->second.AsNumber());
                }

                if (const auto fade = values.find("clipmap_fade_seconds");
                    fade != values.end())
                {
                    if (!fade->second.IsNumber())
                    {
                        throw rpc::Error(
                            kInvalid,
                            "clipmap_fade_seconds must be a number.");
                    }
                    layers.clipmapFadeSeconds =
                        static_cast<f32>(fade->second.AsNumber());
                }

                if (const auto bias = values.find("lod_bias_stops");
                    bias != values.end())
                {
                    if (!bias->second.IsNumber())
                    {
                        throw rpc::Error(
                            kInvalid,
                            "lod_bias_stops must be a number.");
                    }
                    layers.lodBiasStops =
                        static_cast<f32>(bias->second.AsNumber());
                }

                views.SetTerrainLayers(id, layers);
                return LayersToRpc(id, views.TerrainLayers(id));
            }
            catch (const rpc::Error&)
            {
                throw;
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(kFailed, exception.what());
            }
        });

    dispatcher.Register(
        {
            .name = "view.text_diagnostics",
            .description =
                "Returns the full text diagnostic of a Studio RenderView: "
                "camera position/heading/pitch, distance from the planet "
                "core, height above datum (sea level), above the terrain and "
                "above the water surface, and for the point below the camera "
                "(and under the cursor when cursor_u/cursor_v in [0,1] are "
                "given) latitude/longitude, terrain and coarse elevation, "
                "water depth, radius from core, slope, downhill bearing, "
                "climate and biome weights. 'text' is the same multi-line "
                "readout the viewport HUD draws; 'hud' says whether the HUD "
                "is on.",
            .mutating = false
        },
        [&views](const Value& params)
        {
            const auto& values = RequireObject(params);
            const std::string id = RequireString(values, "id");

            std::optional<std::pair<f32, f32>> cursor;
            const auto cursorU = values.find("cursor_u");
            const auto cursorV = values.find("cursor_v");
            if (cursorU != values.end() || cursorV != values.end())
            {
                if (cursorU == values.end() || cursorV == values.end() ||
                    !cursorU->second.IsNumber() ||
                    !cursorV->second.IsNumber())
                {
                    throw rpc::Error(
                        -32602,
                        "cursor_u and cursor_v must both be numbers.");
                }
                cursor = std::pair<f32, f32>{
                    static_cast<f32>(cursorU->second.AsNumber()),
                    static_cast<f32>(cursorV->second.AsNumber())};
            }

            try
            {
                return TextReportToRpc(
                    views.TextDiagnostics(id, cursor),
                    views.TextDiagnosticsHud(id));
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(kInvalid, exception.what());
            }
        });

    dispatcher.Register(
        {
            .name = "view.text_diagnostics_set",
            .description =
                "Turns the viewport's text diagnostics HUD on or off for a "
                "Studio RenderView (the 'Text readout' checkbox in the "
                "viewport Diagnostics properties). Returns the new state.",
            .mutating = true
        },
        [&views](const Value& params)
        {
            const auto& values = RequireObject(params);
            const std::string id = RequireString(values, "id");
            const auto enabled = values.find("enabled");
            if (enabled == values.end() || !enabled->second.IsBool())
            {
                throw rpc::Error(-32602, "enabled must be a boolean.");
            }

            try
            {
                views.SetTextDiagnosticsHud(id, enabled->second.AsBool());
                return Value(Value::Object{
                    {"id", id},
                    {"hud", views.TextDiagnosticsHud(id)}});
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(kFailed, exception.what());
            }
        });

    dispatcher.Register(
        {
            .name = "viewport.pose_get",
            .description =
                "The complete camera pose of a Studio perspective view: "
                "target body object id, whether it uses terrain navigation, "
                "the planet-fixed observer position (metres), the surface "
                "frame (east/north/up) and the free-camera yaw/pitch "
                "(radians). Feed it to viewport.pose_set to put the camera "
                "back exactly. id defaults to studio.primary.",
            .mutating = false
        },
        [&views](const Value& params)
        {
            const std::string id = ViewIdOrPrimary(params);
            const auto pose = views.ViewPose(id);
            if (!pose.has_value())
            {
                throw rpc::Error(
                    kInvalid,
                    "View '" + id + "' has no target body or camera pose.");
            }
            return PoseToRpc(id, *pose);
        });

    dispatcher.Register(
        {
            .name = "viewport.pose_set",
            .description =
                "Restores a camera pose captured by viewport.pose_get "
                "(target_object, terrain, observer, east, north, up, yaw, "
                "pitch). The view's target body must already be "
                "target_object (select it and viewport.focus_body first); "
                "otherwise restored is false. id defaults to "
                "studio.primary.",
            .mutating = true
        },
        [&views](const Value& params)
        {
            const std::string id = ViewIdOrPrimary(params);
            const StudioViewPose pose = PoseFromRpc(RequireObject(params));
            try
            {
                return Value(Value::Object{
                    {"id", id},
                    {"restored", views.RestoreViewPose(id, pose)}});
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(kFailed, exception.what());
            }
        });

    dispatcher.Register(
        {
            .name = "view.zoom_get",
            .description =
                "Camera zoom of a Studio RenderView: a telephoto factor on "
                "the field of view (1 = default, 2 = half the angle). "
                "Range 0.5 to 100. id defaults to studio.primary.",
            .mutating = false
        },
        [&views](const Value& params)
        {
            const std::string id = ViewIdOrPrimary(params);
            try
            {
                return Value(Value::Object{
                    {"id", id},
                    {"zoom", views.Zoom(id)},
                    {"min", StudioRenderViewSet::kMinZoom},
                    {"max", StudioRenderViewSet::kMaxZoom}});
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(kInvalid, exception.what());
            }
        });

    dispatcher.Register(
        {
            .name = "view.zoom_set",
            .description =
                "Sets the camera zoom of a Studio RenderView (the viewport "
                "Zoom control and mouse wheel). zoom is clamped to 0.5..100. "
                "id defaults to studio.primary. Returns the zoom applied.",
            .mutating = true
        },
        [&views](const Value& params)
        {
            const std::string id = ViewIdOrPrimary(params);
            const f64 zoom = NumberFromRpc(RequireObject(params), "zoom");
            try
            {
                views.SetZoom(id, zoom);
                return Value(Value::Object{
                    {"id", id}, {"zoom", views.Zoom(id)}});
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(kFailed, exception.what());
            }
        });
}
} // namespace orbit::studio_ui
