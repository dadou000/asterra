#include <orbit/studio_ui/StudioViewportTextDiagnostics.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <numbers>
#include <string_view>

namespace orbit::studio_ui
{
namespace
{
constexpr f64 kRadiansToDegrees =
    180.0 / std::numbers::pi_v<f64>;

struct NamedWeight
{
    std::string_view name;
    f32 terrain::BiomeWeights::* member;
};

constexpr std::array kBiomeWeights{
    NamedWeight{"ocean", &terrain::BiomeWeights::ocean},
    NamedWeight{"desert", &terrain::BiomeWeights::desert},
    NamedWeight{"grassland", &terrain::BiomeWeights::grassland},
    NamedWeight{"temperate_forest",
        &terrain::BiomeWeights::temperateForest},
    NamedWeight{"boreal_forest", &terrain::BiomeWeights::borealForest},
    NamedWeight{"tundra", &terrain::BiomeWeights::tundra},
    NamedWeight{"alpine", &terrain::BiomeWeights::alpine},
    NamedWeight{"wetland", &terrain::BiomeWeights::wetland}
};

[[nodiscard]] f64 ElevationAt(
    const terrain::TerrainSource& source,
    const world::PlanetId planet,
    const math::Double3& direction,
    const f64 footprintMeters) noexcept
{
    return source.Sample({
        .unitDirection = math::Normalize(direction),
        .footprintMeters = footprintMeters,
        .planet = planet,
        .radialOffsetMeters = 0.0
    }).elevationMeters;
}

[[nodiscard]] std::string Meters(const f64 value)
{
    const f64 magnitude = std::abs(value);
    if (magnitude >= 100'000.0)
    {
        return std::format("{:.3f} km", value / 1000.0);
    }
    if (magnitude >= 1000.0)
    {
        return std::format("{:.2f} m", value);
    }
    return std::format("{:.3f} m", value);
}

void AppendPoint(
    std::string& text,
    const std::string_view indent,
    const StudioTerrainPointReport& point)
{
    text += std::format(
        "{}lat {:+.6f} deg   lon {:+.6f} deg   dir ({:+.6f}, {:+.6f}, {:+.6f})\n",
        indent,
        point.latitudeDegrees,
        point.longitudeDegrees,
        point.unitDirection.x,
        point.unitDirection.y,
        point.unitDirection.z);
    text += std::format(
        "{}terrain {} (coarse {}, detail {:+.3f} m)   sample footprint {}\n",
        indent,
        Meters(point.terrainElevationMeters),
        Meters(point.coarseElevationMeters),
        point.detailDeltaMeters,
        Meters(point.footprintMeters));
    if (point.standingWaterDepthMeters > 0.0)
    {
        text += std::format(
            "{}water depth {}   water surface {}   (submerged)\n",
            indent,
            Meters(point.standingWaterDepthMeters),
            Meters(point.waterSurfaceElevationMeters));
    }
    else
    {
        text += std::format(
            "{}water none   {} above the water datum\n",
            indent,
            Meters(point.terrainElevationMeters));
    }
    text += std::format(
        "{}radius from core: ground {}   rendered surface {}\n",
        indent,
        Meters(point.groundRadiusFromCoreMeters),
        Meters(point.renderedSurfaceRadiusFromCoreMeters));
    text += std::format(
        "{}slope {:.2f} deg   downhill bearing {:.1f} deg\n",
        indent,
        point.slopeDegrees,
        point.downhillBearingDegrees);
    text += std::format(
        "{}climate  temp {:.1f} C   humidity {:.3f}   precip {:.3f}   continentality {:.3f}\n",
        indent,
        point.climate.temperatureC,
        point.climate.humidity,
        point.climate.precipitation,
        point.climate.continentality);

    std::string weights;
    for (const auto& entry : kBiomeWeights)
    {
        const f32 weight = point.biomes.*entry.member;
        if (weight < 0.0005F)
        {
            continue;
        }
        if (!weights.empty())
        {
            weights += "  ";
        }
        weights += std::format("{} {:.3f}", entry.name, weight);
    }
    text += std::format(
        "{}biome {}   weights: {}\n",
        indent,
        point.dominantBiome,
        weights.empty() ? std::string("none") : weights);
}
} // namespace

f64 StudioPixelFootprintMeters(
    const f64 verticalFovRadians,
    const u32 viewportHeight,
    const f64 distanceMeters) noexcept
{
    if (viewportHeight == 0U ||
        !std::isfinite(distanceMeters) ||
        distanceMeters <= 0.0)
    {
        return 1.0;
    }

    const f64 halfFov = std::clamp(verticalFovRadians * 0.5, 1.0e-4, 1.55);
    return std::max(
        2.0 * distanceMeters * std::tan(halfFov) /
            static_cast<f64>(viewportHeight),
        0.25);
}

StudioTerrainPointReport SampleStudioTerrainPoint(
    const terrain::TerrainSource& source,
    const world::PlanetId planet,
    const f64 planetRadiusMeters,
    const math::Double3& unitDirection,
    const f64 footprintMeters)
{
    StudioTerrainPointReport report;
    report.unitDirection = math::Normalize(unitDirection);
    report.footprintMeters = std::max(footprintMeters, 0.25);

    const auto& direction = report.unitDirection;
    report.latitudeDegrees =
        std::asin(std::clamp(direction.y, -1.0, 1.0)) * kRadiansToDegrees;
    report.longitudeDegrees =
        std::atan2(direction.z, direction.x) * kRadiansToDegrees;

    const auto sample = source.Sample({
        .unitDirection = direction,
        .footprintMeters = report.footprintMeters,
        .planet = planet,
        .radialOffsetMeters = 0.0
    });

    report.terrainElevationMeters = sample.elevationMeters;
    report.coarseElevationMeters = sample.coarseElevationMeters;
    report.detailDeltaMeters =
        sample.elevationMeters - sample.coarseElevationMeters;
    report.standingWaterDepthMeters =
        std::max(sample.standingWaterDepthMeters, 0.0);
    report.waterSurfaceElevationMeters =
        sample.elevationMeters + report.standingWaterDepthMeters;
    report.underwater = report.standingWaterDepthMeters > 0.0;
    report.groundRadiusFromCoreMeters =
        planetRadiusMeters + sample.elevationMeters;
    report.renderedSurfaceRadiusFromCoreMeters =
        planetRadiusMeters + report.waterSurfaceElevationMeters;
    report.climate = sample.climate;
    report.biomes = terrain::NormalizeBiomeWeights(sample.biomes);

    f32 best = -1.0F;
    for (const auto& entry : kBiomeWeights)
    {
        const f32 weight = report.biomes.*entry.member;
        if (weight > best)
        {
            best = weight;
            report.dominantBiome = std::string(entry.name);
        }
    }

    // Slope from a central difference on the same source, so it reflects the
    // relief that is actually resolved at this footprint.
    const math::Double3 pole{0.0, 1.0, 0.0};
    math::Double3 east = math::Cross(direction, pole);
    if (math::LengthSquared(east) < 1.0e-18)
    {
        east = {0.0, 0.0, 1.0};
    }
    east = math::Normalize(east);
    math::Double3 north =
        pole - direction * math::Dot(pole, direction);
    if (math::LengthSquared(north) < 1.0e-18)
    {
        north = {1.0, 0.0, 0.0};
    }
    north = math::Normalize(north);

    const f64 step =
        std::max(report.footprintMeters, 2.0);
    const f64 angle = step / std::max(planetRadiusMeters, 1.0);
    const auto elevationOffset =
        [&](const math::Double3& axis, const f64 sign)
    {
        return ElevationAt(
            source,
            planet,
            direction + axis * (sign * angle),
            report.footprintMeters);
    };

    const f64 gradientEast =
        (elevationOffset(east, 1.0) - elevationOffset(east, -1.0)) /
        (2.0 * step);
    const f64 gradientNorth =
        (elevationOffset(north, 1.0) - elevationOffset(north, -1.0)) /
        (2.0 * step);

    report.slopeDegrees =
        std::atan(std::hypot(gradientEast, gradientNorth)) *
        kRadiansToDegrees;
    f64 bearing =
        std::atan2(-gradientEast, -gradientNorth) * kRadiansToDegrees;
    if (bearing < 0.0)
    {
        bearing += 360.0;
    }
    report.downhillBearingDegrees = bearing;
    return report;
}

std::string FormatStudioViewportTextReport(
    const StudioViewportTextReport& report)
{
    std::string text;
    text += std::format(
        "View {}  [{}]  {}x{}  navigation: {}\n",
        report.viewId,
        report.viewMode,
        report.width,
        report.height,
        report.navigation);
    text += std::format(
        "Camera pos ({:.3f}, {:.3f}, {:.3f}) m   heading {:.1f} deg   pitch {:+.1f} deg\n",
        report.cameraPosition.x,
        report.cameraPosition.y,
        report.cameraPosition.z,
        report.cameraHeadingDegrees,
        report.cameraPitchDegrees);
    text += std::format(
        "       fov {:.1f} deg   near {}   far {}\n",
        report.verticalFovDegrees,
        Meters(report.nearPlaneMeters),
        Meters(report.farPlaneMeters));

    text += std::format(
        "Layers: production {}{}  orbital globe {}  ocean {}  surface effects {}   LOD bias {:+.2f} stops\n",
        report.layers.productionSurface ? "on" : "OFF",
        report.layers.fullClipmap ? " (full clipmap, ground to orbit)" : "",
        report.layers.macroGlobe && !report.layers.fullClipmap ? "on" : "OFF",
        report.layers.ocean ? "on" : "OFF",
        report.layers.surfaceEffects ? "on" : "OFF",
        static_cast<f64>(report.layers.lodBiasStops));

    if (!report.hasTerrain)
    {
        text += std::format(
            "Distance from core {}   (no terrain on this target)\n",
            Meters(report.distanceFromCoreMeters));
        return text;
    }

    text += std::format(
        "Planet radius {}   distance from core {}\n",
        Meters(report.planetRadiusMeters),
        Meters(report.distanceFromCoreMeters));
    text += std::format(
        "Height above datum (sea level) {}",
        Meters(report.heightAboveDatumMeters));
    if (report.heightAboveTerrainMeters.has_value())
    {
        text += std::format(
            "   above terrain {}",
            Meters(*report.heightAboveTerrainMeters));
    }
    if (report.heightAboveWaterSurfaceMeters.has_value())
    {
        text += std::format(
            "   above water surface {}",
            Meters(*report.heightAboveWaterSurfaceMeters));
    }
    text += "\n";

    if (report.nadir.has_value())
    {
        text += "Below camera:\n";
        AppendPoint(text, "  ", *report.nadir);
    }

    if (report.cursor.has_value())
    {
        const auto& cursor = *report.cursor;
        text += std::format(
            "Under cursor (hit {} away, pick ground {} rendered {}):\n",
            Meters(cursor.hitDistanceMeters),
            Meters(cursor.pickPhysicalElevationMeters),
            Meters(cursor.pickRenderedElevationMeters));
        AppendPoint(text, "  ", cursor.point);
        if (!cursor.physicalPage.empty())
        {
            text += std::format(
                "  physical page {}{}\n",
                cursor.physicalPage,
                cursor.physicalLod.has_value()
                    ? std::format(" (LOD {})", *cursor.physicalLod)
                    : std::string());
        }
    }

    if (report.clipmapPlan.has_value())
    {
        const auto& plan = *report.clipmapPlan;
        if (plan.frozen || plan.wireframe)
        {
            text += std::format(
                "Clipmap debug:{}{}\n",
                plan.frozen ? " FROZEN (the camera no longer drives the clipmap)" : "",
                plan.wireframe ? " wireframe" : "");
        }
        if (plan.dynamic)
        {
            text += std::format(
                "Clipmap{}: levels {}..{} of {} active (finest {}, reaches {})   nearest ground {}, screen wants {} spacing   {} plan changes\n",
                plan.banded ? " (EXPERIMENT, distance bands)" : "",
                plan.firstLevel,
                plan.lastLevel,
                plan.ladderLevels,
                Meters(plan.finestSpacingMeters),
                Meters(plan.coarsestHalfExtentMeters),
                Meters(plan.nearestGroundMeters),
                Meters(plan.requiredSpacingMeters),
                plan.planChanges);
            // One compact entry per drawn level: index and sample spacing (and the
            // camera-distance band of the experimental banded clipmap).
            std::string levels = plan.banded ? "  bands:" : "  levels:";
            for (const auto& level : plan.levels)
            {
                if (plan.banded)
                {
                    levels += std::format(
                        " L{}={}..{} @{}",
                        level.level,
                        Meters(level.bandInnerMeters),
                        Meters(level.bandOuterMeters),
                        Meters(level.spacingMeters));
                }
                else
                {
                    levels += std::format(
                        " L{}={}", level.level, Meters(level.spacingMeters));
                }
            }
            text += levels + '\n';
        }
        else
        {
            text += std::format(
                "Clipmap: all {} levels active (dynamic planning off)\n",
                plan.ladderLevels);
        }
    }

    if (report.cpuTerrain.has_value())
    {
        const auto& cpu = *report.cpuTerrain;
        text += std::format(
            "CPU terrain: page pool {} workers, {} running, {} queued   orbital patches {} building, {} resident\n",
            cpu.poolWorkers,
            cpu.poolRunningJobs,
            cpu.poolQueuedJobs,
            cpu.globePatchesPending,
            cpu.globePatchesResident);
        if (cpu.hasPages)
        {
            text += std::format(
                "  pages [{}] {} total: {} dirty, {} queued, {} building, {} uploading, {} ready, {} failed, {} stale   products {}/{}\n",
                cpu.pageState,
                cpu.pages,
                cpu.dirtyPages,
                cpu.queuedPages,
                cpu.buildingPages,
                cpu.uploadingPages,
                cpu.readyPages,
                cpu.failedPages,
                cpu.stalePages,
                cpu.completedProducts,
                cpu.totalProducts);
        }
    }

    text += std::format(
        "Terrain runtime: page level {}   coverage tier {}   source rev {}   world gen {}   runtime gen {}\n",
        report.physicalPageLevel,
        report.adaptiveCoverageTier,
        report.terrainSourceRevision,
        report.worldGeneration,
        report.runtimeGeneration);
    return text;
}
} // namespace orbit::studio_ui
