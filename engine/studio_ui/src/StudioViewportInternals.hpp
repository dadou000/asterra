#pragma once

#include <orbit/studio_ui/StudioViewportRenderer.hpp>
#include <orbit/studio_ui/LightingInteractionState.hpp>
#include <orbit/studio_ui/LightingSelectionInspection.hpp>
#include <orbit/studio_ui/StudioLightingOverlayGeometry.hpp>
#include <orbit/studio_ui/StudioRuntimeProfiler.hpp>

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>
#include <vector>
#include <orbit/profiler/Profiler.hpp>
#include <orbit/celestial_atmosphere/SkyIrradiance.hpp>
#include <orbit/celestial_radiometry/Radiometry.hpp>
#include <orbit/content/ContentService.hpp>
#include <orbit/lighting/MaterialEmission.hpp>
#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/studio_ui/StudioTerrainDiagnosticOverlayGeometry.hpp>
#include <orbit/studio_ui/StudioTerrainOverlayGeometry.hpp>
#include <orbit/studio_ui/VolumeSurfaceEffectRenderBridge.hpp>
#include <orbit/studio_ui/VolumeParticleRenderBridge.hpp>
#include <orbit/world_model/CelestialAtmosphereBinding.hpp>
#include <orbit/world_model/CelestialCloudBinding.hpp>
#include <orbit/world_model/CelestialGiantBinding.hpp>
#include <orbit/world_model/CelestialCompactObjectBinding.hpp>
#include <orbit/world_model/CelestialMagnetosphereBinding.hpp>
#include <orbit/world_model/CelestialSmallBodyBinding.hpp>
#include <orbit/world_model/CelestialOceanBinding.hpp>
#include <orbit/world_model/CelestialRadiometryBinding.hpp>
#include <orbit/world_model/CelestialRingBinding.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>
#include <orbit/world_model/LocalLightBinding.hpp>
#include <orbit/world_model/StaticMeshBinding.hpp>
#include <orbit/world_model/VisibilityProxyBinding.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>
#include <orbit/lighting/LocalLightRegistry.hpp>
#include <orbit/lighting/AnalyticBodyVisibility.hpp>
#include <orbit/lighting/TerrainHeightfieldVisibility.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain_gpu/GpuPhysicalPageComposite.hpp>
#include <orbit/terrain_gpu/PersistentGpuTerrainCache.hpp>
#include <orbit/world/PlanetTileNeighborhood.hpp>
#include <filesystem>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <sstream>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace orbit::studio_ui::viewport_detail
{

// Resolves a view's cloud lab options into the renderer's lab: places the cloud
// ahead of the camera along the ground when the place serial changes, and builds
// the lab sun (elevation/azimuth at the cloud) when the sun is overridden.
template <typename Camera, typename Anchor>
[[nodiscard]] celestial_clouds::CloudLab ResolveCloudLab(
    Anchor& anchor,
    const StudioCloudLab& options,
    const Camera& camera,
    const f64 planetRadiusMeters,
    math::Float3& sunDirection,
    bool& sunOverridden)
{
    sunOverridden = false;
    if (!options.enabled)
    {
        anchor.valid = false;
        return {};
    }

    const math::Double3 position = camera.localPositionMeters;
    if (!anchor.valid || anchor.serial != options.placeSerial)
    {
        const math::Double3 up = math::Normalize(position);
        const math::Double3 forward{
            static_cast<f64>(camera.forward.x),
            static_cast<f64>(camera.forward.y),
            static_cast<f64>(camera.forward.z)};
        math::Double3 horizontal = forward - up * math::Dot(forward, up);
        if (math::Length(horizontal) < 1.0e-6)
        {
            horizontal = math::Cross(math::Double3{0.0, 1.0, 0.0}, up);
        }
        horizontal = math::Normalize(horizontal);
        anchor.center = math::Normalize(
            up * planetRadiusMeters +
            horizontal * static_cast<f64>(options.distanceMeters));
        anchor.serial = options.placeSerial;
        anchor.valid = true;
    }

    if (options.overrideSun)
    {
        const math::Double3 up = anchor.center;
        math::Double3 east = math::Cross(math::Double3{0.0, 1.0, 0.0}, up);
        if (math::Length(east) < 1.0e-6)
        {
            east = math::Double3{1.0, 0.0, 0.0};
        }
        east = math::Normalize(east);
        const math::Double3 north = math::Cross(up, east);
        const f64 elevation =
            static_cast<f64>(options.sunElevationDegrees) * std::numbers::pi / 180.0;
        const f64 azimuth =
            static_cast<f64>(options.sunAzimuthDegrees) * std::numbers::pi / 180.0;
        const math::Double3 sun =
            up * std::sin(elevation) +
            (north * std::cos(azimuth) + east * std::sin(azimuth)) * std::cos(elevation);
        sunDirection = {
            static_cast<f32>(sun.x), static_cast<f32>(sun.y), static_cast<f32>(sun.z)};
        sunOverridden = true;
    }

    return celestial_clouds::CloudLab{
        .enabled = true,
        .centerDirection = anchor.center,
        .radiusMeters = options.radiusMeters,
        .type = options.type,
        .coverage = options.coverage,
        .cirrus = options.cirrus,
        .precipitation = options.precipitation,
        .heightScale = options.heightScale,
        .maturity = options.maturity,
        .organisation = options.organisation,
        .density = options.density,
        .cirrusSheet = options.cirrusSheet,
        .seed = options.seed};
}

// Scene-linear reference: stellar irradiance is expressed as a fraction of the
// solar constant, so a white Lambertian surface at 1 AU has radiance 1/pi.
constexpr f64 kStudioReferenceIrradianceWattsPerSquareMeter =
    1361.0;

struct StudioPhysicalRenderPages
{
    std::vector<
        terrain_gpu::GpuPhysicalSurfacePage>
        pages;
    u64 generation{
        0x4D31325048595352ULL};
};

struct ResolvedStudioDirectLight
{
    math::Float3 directionBody{
        0.55F, 0.72F, -0.48F};
    f32 irradianceScale{0.0F};
    std::optional<
        world_model::DirectBodyLighting>
        direct;
};

[[nodiscard]] u64 CombineFingerprint(
    const u64 seed,
    const u64 value) noexcept;

[[nodiscard]] u64 StableViewportHash(
    const std::string_view value) noexcept;

[[nodiscard]]
celestial_scheduler::WorkKey
CelestialWorkKeyFor(
    const std::string_view viewportId,
    const universe::BodyId body,
    const celestial_scheduler::WorkKind kind) noexcept;

[[nodiscard]] u64 QuantizedLightingFingerprintValue(
    const f32 value,
    const f32 quantum) noexcept;

[[nodiscard]] math::Float3 AtmosphereSkyIrradianceSummary(
    const celestial_atmosphere::AtmosphereSkyView* sky) noexcept;

[[nodiscard]] bool SameClipmapConfig(
    const terrain_view::ClipmapConfig& a,
    const terrain_view::ClipmapConfig& b) noexcept;

[[nodiscard]] terrain_view::ClipmapConfig EffectiveClipmapConfig(
    const terrain_view::ClipmapConfig& base,
    const StudioTerrainLayerOptions& layers);

[[nodiscard]] math::Double3x3 EulerDegreesToRotation(
    const math::Double3& eulerDegrees) noexcept;

[[nodiscard]] std::vector<lighting::VisibilityProxy>
BuildVisibilityProxies(
    const std::vector<
        world_model::ResolvedVisibilityProxy>& resolved,
    const universe::BodyId body,
    const frames::FrameId bodyFrame);

[[nodiscard]] std::vector<editor_ui::PreviewLine>
SelectedMagnetosphereDiagnosticLines(
    studio_session::StudioSession& session,
    const world_model::ResolvedMagnetosphere& resolved,
    const f64 referenceRadiusMeters,
    const render_view::CameraState& camera);

[[nodiscard]] std::vector<editor_ui::PreviewLine>
SelectedLocalLightGizmoLines(
    studio_session::StudioSession& session,
    const render_view::CameraState& camera);

[[nodiscard]] std::vector<editor_ui::PreviewLine>
VolumeInputGizmoLines(
    studio_session::StudioSession& session,
    const render_view::CameraState& camera,
    const bool showAll);

[[nodiscard]] std::vector<editor_ui::PreviewLine>
VolumeSlicePlaneLines(
    const world_model::ResolvedVolumeDomain& volume,
    const render_view::CameraState& camera,
    const volume_solver::VolumeSliceAxis axis,
    const u32 sliceIndex);

[[nodiscard]] std::vector<editor_ui::PreviewLine>
SelectedVolumeDomainLines(
    studio_session::StudioSession& session,
    const render_view::CameraState& camera);

[[nodiscard]] std::optional<StudioTerrainAuthoringOverlay>
SelectedTerrainAuthoringOverlay(
    studio_session::StudioSession& session,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime);

[[nodiscard]] std::vector<StudioTerrainAuthoringOverlay>
TerrainConstraintDiagnosticOverlays(
    studio_session::StudioSession& session,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime);

[[nodiscard]] terrain_render::TerrainPreviewCamera
TerrainCameraFromBodyCamera(
    const render_view::CameraState& camera,
    const world::SurfaceFrame& frame);

[[nodiscard]] StudioPhysicalRenderPages
BuildPhysicalRenderPages(
    studio_session::StudioSession& session,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime,
    const terrain::AnalyticTerrainSource& analytic,
    rhi::Device& device);

[[nodiscard]] f64 ReferenceRadiusForShape(
    const universe::BodyShape& shape);

[[nodiscard]] ResolvedStudioDirectLight
ResolveStudioDirectLight(
    studio_session::StudioSession& session,
    const universe::BodyId receiver,
    const time::SimulationTime atTime);

[[nodiscard]] std::vector<celestial_far_render::FarBodyDraw>
ResolveSystemBodyDraws(
    studio_session::StudioSession& session,
    content::ContentService* content,
    const universe::BodyId activeBody,
    const render_view::CameraState& camera,
    const time::SimulationTime atTime,
    const u32 width,
    const u32 height);

[[nodiscard]] std::vector<lighting::ResolvedLocalLight>
ResolveStudioLocalLights(
    studio_session::StudioSession& session,
    const lighting::LightingView& lightingView,
    const std::optional<scene::ObjectId> root);

[[nodiscard]] std::vector<scene::ObjectId>
CollectVolumeObjects(
    const scene::ObjectStore& objects);

[[nodiscard]] std::vector<lighting::EmissiveVolumeSource>
ResolveAuthoredEmissiveVolumes(
    const scene::ObjectStore& objects);

[[nodiscard]] editor_ui::PreviewMaterial
ResolveRuntimeMaterialAsset(
    content::ContentService& content,
    const content::AssetRecord* asset,
    const u32 depth = 0U);

[[nodiscard]] std::optional<editor_ui::PreviewMaterial>
ResolveRuntimeBodyMaterialIfAssigned(
    content::ContentService* content,
    scene::ObjectStore& objects,
    const std::optional<scene::ObjectId> bodyObject);

[[nodiscard]] editor_ui::PreviewMaterial
ResolveRuntimeBodyMaterial(
    content::ContentService* content,
    scene::ObjectStore& objects,
    const std::optional<scene::ObjectId> bodyObject);
[[nodiscard]] f64 QuantizedSkyObserverRadius(
    const f64 observerRadiusMeters,
    const f64 bottomRadiusMeters) noexcept;

} // namespace orbit::studio_ui::viewport_detail

namespace orbit::studio_ui
{
// Borrowed frame inputs. The context is consumed synchronously; render-graph
// callbacks copy the values and resource owners they need for later execution.
struct StudioScenePassContext
{
    render_graph::RenderGraph& graph;
    StudioRenderViewSet& views;
    studio_session::StudioSession& session;
    render_view::RenderView* view;
    const StudioRenderViewInfo& info;
    const render_view::ImportedTargets& targets;
    const std::string& prefix;
    const std::optional<studio_session::StudioTerrainViewportRuntimeSnapshot>& terrainRuntime;
    f64& nearFieldWaterWeight;
    f32& nearFieldSeaLevelMeters;
    bool hasMacroGlobe;
    viewport_detail::ResolvedStudioDirectLight& studioDirectLight;
    const StudioComposeCpuTimingRecorder& cpuTimingRecorder;
    std::chrono::steady_clock::time_point& composeStageStarted;
    const std::optional<world_model::ResolvedOceanBody>& resolvedOceanForView;
    u32 frameIndex;
    std::function<void(rhi::CommandList&)> drawBackgroundBodies;
    std::function<void(std::string_view)> recordComposeStage;
    const surface::TerrainSurfaceCapability* macroGlobeSurface;
    const std::optional<universe::BodyShape>& shape;
    std::function<bool(std::string_view, universe::BodyId,
        celestial_scheduler::WorkKind, u64, celestial_scheduler::WorkBackend,
        u32, i32, bool)> acquireCelestialGrant;
    std::function<bool(std::string_view, universe::BodyId,
        celestial_scheduler::WorkKind, u64)> completeCelestialGrant;
};
} // namespace orbit::studio_ui

namespace orbit::studio_ui
{
struct StudioLightingPassContext
{
    render_graph::RenderGraph& graph;
    studio_session::StudioSession& session;
    render_view::RenderView* view;
    const StudioRenderViewInfo& info;
    const render_view::ImportedTargets& targets;
    const std::string& prefix;
    StudioViewportPresentation presentation;
    const studio_session::StudioRuntimeSnapshot& snapshot;
    time::SimulationTime atTime;
    u32 frameIndex;
    const viewport_detail::ResolvedStudioDirectLight& studioDirectLight;
    const std::optional<studio_session::StudioTerrainViewportRuntimeSnapshot>& terrainRuntime;
    const lighting::LightingWorkPlan& lightingPlan;
    lighting::LightingTimestampRecorder* lightingTimestamps;
    const StudioComposeCpuTimingRecorder& cpuTimingRecorder;
    std::function<void(std::string_view)> recordComposeStage;
    render_graph::BufferHandle& sharedRadianceCellsHandle;
    render_graph::BufferHandle& sharedRadianceLevelsHandle;
    u32& sharedRadianceLevelCount;
    const std::optional<universe::BodyShape>& shape;
    const universe::BodyRegistry* bodies;
    const frames::FrameGraph* frames;
    const std::optional<world_model::ResolvedMagnetosphere>& resolvedMagnetosphereForView;
    std::chrono::steady_clock::time_point& composeStageStarted;
    f64 nearFieldWaterWeight;
    const std::optional<world_model::ResolvedOceanBody>& resolvedOceanForView;
    f32 nearFieldSeaLevelMeters;
};
} // namespace orbit::studio_ui
