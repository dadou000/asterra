#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/studio_ui/StudioTerrainLayerOptions.hpp>
#include <orbit/terrain/TerrainFields.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/world/Planet.hpp>

#include <optional>
#include <string>

namespace orbit::studio_ui
{
// Everything the terrain source says about one point on a planet surface.
// Heights are metres. "Datum" is the planet reference radius: elevation 0 is
// the undisturbed sea level that standing water is measured against.
struct StudioTerrainPointReport
{
    math::Double3 unitDirection{0.0, 1.0, 0.0};
    // +Y is the pole. Longitude is atan2(z, x): 0 on +X, increasing toward +Z.
    f64 latitudeDegrees{0.0};
    f64 longitudeDegrees{0.0};
    // Sample support used, i.e. the terrain detail that was resolved.
    f64 footprintMeters{1.0};

    f64 terrainElevationMeters{0.0};
    f64 coarseElevationMeters{0.0};
    // Elevation minus the coarse (pre-detail) elevation: the fine relief.
    f64 detailDeltaMeters{0.0};
    f64 standingWaterDepthMeters{0.0};
    f64 waterSurfaceElevationMeters{0.0};
    bool underwater{false};

    f64 groundRadiusFromCoreMeters{0.0};
    f64 renderedSurfaceRadiusFromCoreMeters{0.0};

    f64 slopeDegrees{0.0};
    // Compass bearing of the downhill direction: 0 north, 90 east.
    f64 downhillBearingDegrees{0.0};

    terrain::TerrainClimate climate{};
    // Normalised so the weights sum to one.
    terrain::BiomeWeights biomes{};
    std::string dominantBiome;
};

struct StudioCursorPickReport
{
    StudioTerrainPointReport point;
    f64 hitDistanceMeters{0.0};
    f64 pickPhysicalElevationMeters{0.0};
    f64 pickRenderedElevationMeters{0.0};
    std::optional<u8> physicalLod;
    // "face/level/x/y" of the physical page under the cursor.
    std::string physicalPage;
};

// Terrain work running on CPU worker threads: the physical-page build pool and
// the orbital patch builds. All values are a snapshot; none is authoritative.
struct StudioCpuTerrainReport
{
    // Physical-page pool ("Orbit.TerrainPages.*").
    u32 poolWorkers{0U};
    u64 poolRunningJobs{0U};
    u64 poolQueuedJobs{0U};
    u64 poolOutstandingJobs{0U};

    // Page rebuild pipeline for the viewed body.
    bool hasPages{false};
    std::string pageState;
    u32 pages{0U};
    u32 dirtyPages{0U};
    u32 queuedPages{0U};
    u32 buildingPages{0U};
    u32 uploadingPages{0U};
    u32 readyPages{0U};
    u32 failedPages{0U};
    u32 stalePages{0U};
    u32 completedProducts{0U};
    u32 totalProducts{0U};

    // Orbital patches ("Orbit.GlobePatch"): builds in flight / GPU-resident.
    u32 globePatchesPending{0U};
    u32 globePatchesResident{0U};
};

struct StudioViewportTextReport
{
    std::string viewId;
    std::string viewMode;
    // "terrain", "reference_sphere" or "none".
    std::string navigation;
    u32 width{0U};
    u32 height{0U};

    // Camera, in the planet-fixed frame of the viewport's target body.
    math::Double3 cameraPosition{};
    math::Double3 forward{};
    math::Double3 up{};
    f64 verticalFovDegrees{0.0};
    f64 nearPlaneMeters{0.0};
    f64 farPlaneMeters{0.0};

    bool hasTerrain{false};
    f64 planetRadiusMeters{0.0};
    f64 distanceFromCoreMeters{0.0};
    f64 heightAboveDatumMeters{0.0};
    // Straight down from the camera. Empty when there is no terrain.
    std::optional<f64> heightAboveTerrainMeters;
    std::optional<f64> heightAboveWaterSurfaceMeters;
    f64 cameraPitchDegrees{0.0};
    f64 cameraHeadingDegrees{0.0};

    u8 physicalPageLevel{0U};
    u32 adaptiveCoverageTier{0U};
    u64 terrainSourceRevision{0U};
    u64 worldGeneration{0U};
    u64 runtimeGeneration{0U};

    std::optional<StudioTerrainPointReport> nadir;
    std::optional<StudioCursorPickReport> cursor;
    std::optional<StudioCpuTerrainReport> cpuTerrain;
    StudioTerrainLayerOptions layers{};
};

// Samples the terrain source at one direction. Safe on any thread.
[[nodiscard]] StudioTerrainPointReport SampleStudioTerrainPoint(
    const terrain::TerrainSource& source,
    world::PlanetId planet,
    f64 planetRadiusMeters,
    const math::Double3& unitDirection,
    f64 footprintMeters);

// Terrain footprint a screen pixel covers at a distance from the camera.
[[nodiscard]] f64 StudioPixelFootprintMeters(
    f64 verticalFovRadians,
    u32 viewportHeight,
    f64 distanceMeters) noexcept;

// Human-readable multi-line form, the same text the viewport HUD draws.
[[nodiscard]] std::string FormatStudioViewportTextReport(
    const StudioViewportTextReport& report);
} // namespace orbit::studio_ui
