#pragma once

#include <orbit/celestial_appearance/PlanetaryAppearance.hpp>
#include <orbit/celestial_atmosphere/Atmosphere.hpp>
#include <orbit/celestial_clouds/CloudField.hpp>
#include <orbit/celestial_ocean/OceanOptics.hpp>
#include <orbit/celestial_globe/MacroGlobe.hpp>
#include <orbit/celestial_giants/GiantAppearance.hpp>
#include <orbit/celestial_far_render/FarBodyRenderer.hpp>
#include <orbit/celestial_representation/RepresentationTracker.hpp>
#include <orbit/celestial_rings/RingSystem.hpp>
#include <orbit/editor_ui/BodyPreviewRenderer.hpp>
#include <orbit/editor_ui/PathPreviewRenderer.hpp>
#include <orbit/lighting/DirectLighting.hpp>
#include <orbit/lighting/HardwareRayQueryVisibility.hpp>
#include <orbit/lighting/SoftwareProxyVisibility.hpp>
#include <orbit/lighting/SurfaceDebugRenderer.hpp>
#include <orbit/post_process/ColorLut.hpp>
#include <orbit/post_process/DisplayResolve.hpp>
#include <orbit/render_graph/RenderGraph.hpp>
#include <orbit/render_view/RenderView.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/studio_session/StudioRuntimeBinding.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/studio_ui/StudioRenderViewSet.hpp>
#include <orbit/terrain_debug/TerrainDebugTexture.hpp>
#include <orbit/terrain_gpu/GpuFieldGenerator.hpp>
#include <orbit/terrain_render/TerrainPreviewRenderer.hpp>
#include <orbit/time/SimulationTime.hpp>
#include <orbit/world_model/CelestialLightingService.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::studio_ui
{
struct StudioRenderedView
{
    std::string id;
    render_view::ImportedTargets targets{};
    bool targeted{false};
};

struct StudioMacroGlobeDiagnostics
{
    universe::BodyId body{};
    u64 terrainRevision{0};
    u64 geometryFingerprint{0};
    u64 appearanceFingerprint{0};
    u32 appearanceTexels{0};
};

struct StudioAtmosphereDiagnostics
{
    universe::BodyId body{};
    u64 staticFingerprint{0};
    u64 skyFingerprint{0};
    f64 observerRadiusMeters{0.0};
    f64 observerAltitudeMeters{0.0};
    f64 directIrradianceWattsPerSquareMeter{0.0};
    u32 transmittanceWidth{0};
    u32 transmittanceHeight{0};
    u32 multiScatteringWidth{0};
    u32 multiScatteringHeight{0};
    u32 skyViewWidth{0};
    u32 skyViewHeight{0};
};

struct StudioGiantDiagnostics
{
    universe::BodyId body{};
    u64 appearanceFingerprint{0};
    bool iceGiant{false};
    f64 bandFrequency{0.0};
    f64 bandStrength{0.0};
    f64 stormStrength{0.0};
    f64 projectedRadiusPixels{0.0};
    celestial_representation::Representation representation{
        celestial_representation::Representation::SmoothGlobe};
};

struct StudioStellarDiagnostics
{
    universe::BodyId body{};
    u64 appearanceFingerprint{0};
    f64 effectiveTemperatureKelvin{0.0};
    math::Float3 colorLinear{1.0F, 1.0F, 1.0F};
    f64 projectedRadiusPixels{0.0};
    celestial_representation::Representation representation{
        celestial_representation::Representation::SmoothGlobe};
    f32 resolvedSceneIntensity{0.0F};
    f32 pointSceneIntensity{0.0F};
};

struct StudioRingDiagnostics
{
    universe::BodyId body{};
    u64 fingerprint{0};
    u32 bandCount{0};
    f64 innerRadiusMeters{0.0};
    f64 outerRadiusMeters{0.0};
    f64 projectedOuterRadiusPixels{0.0};
    bool nearRepresentation{false};
    u32 angularSegments{0};
    u32 farProfileSamples{0};
};

struct StudioOceanDiagnostics
{
    universe::BodyId body{};
    u64 opticalFingerprint{0};
    f64 refractiveIndex{1.333};
    f64 orbitalRoughness{0.12};
    f64 glintStrength{1.0};
    f32 oceanFraction{0.0F};
};

struct StudioCloudDiagnostics
{
    universe::BodyId body{};
    u64 fingerprint{0};
    u64 climateRevision{0};
    i64 timeBucket{0};
    u32 layerCount{0};
    f64 meanCoverage{0.0};
    f64 meanOpticalDepth{0.0};
    bool gpuResident{false};
};

struct StudioCelestialLightingDiagnostics
{
    universe::BodyId receiver{};
    universe::BodyId emitter{};
    f64 visibleFraction{1.0};
    f64 irradianceWattsPerSquareMeter{0.0};
    u32 contributingOccluders{0};
};

struct StudioVisibilityProxyDiagnostics
{
    universe::BodyId body{};
    frames::FrameId frame{};
    u64 semanticRevision{0U};
    u32 proxyCount{0U};
    u32 bvhNodeCount{0U};
    u32 dynamicProxyCount{0U};
    f32 maximumNominalErrorMeters{0.0F};
    bool rebuiltThisFrame{false};
    bool hardwareRayQuerySupported{false};
    bool hardwareRayQueryReady{false};
    u32 hardwarePrimitiveCount{0U};
};

struct StudioSurfaceGlobeTransitionDiagnostics
{
    universe::BodyId body{};
    celestial_representation::Representation representation{
        celestial_representation::Representation::ProductionSurface};
    celestial_representation::Representation lowerFidelityNeighbor{
        celestial_representation::Representation::MacroDisplacedGlobe};
    f64 productionSurfaceWeight{1.0};
    f64 macroGlobeWeight{0.0};
    f64 projectedRadiusPixels{0.0};
    f64 productionDetailErrorPixels{0.0};
    f64 macroDisplacementErrorPixels{0.0};
    bool hysteresisHeld{false};
    bool overlapping{false};
};

enum class StudioViewportPresentation : u8
{
    Blank,
    BodyPreview,
    MacroGlobe,
    ProductionTerrain,
    TerrainDebug,
    TerrainDebugUnavailable
};

// Debug mode is intentionally exclusive: when the selected physical page or
// field is unavailable, Studio shows an unavailable debug surface instead of
// silently falling back to the normal body preview.
[[nodiscard]] constexpr StudioViewportPresentation
SelectStudioViewportPresentation(
    const studio_session::ViewportMode mode,
    const bool hasBody,
    const bool hasTerrainRuntime,
    const bool hasMacroGlobe,
    const bool hasLiveDebugPage,
    const bool hasSelectedDebugField) noexcept
{
    if (mode == studio_session::ViewportMode::Debug)
    {
        return hasLiveDebugPage &&
                       hasSelectedDebugField
            ? StudioViewportPresentation::TerrainDebug
            : StudioViewportPresentation::TerrainDebugUnavailable;
    }

    if (mode == studio_session::ViewportMode::Perspective &&
        hasTerrainRuntime)
    {
        return StudioViewportPresentation::ProductionTerrain;
    }

    if (mode == studio_session::ViewportMode::BodyMap &&
        hasBody &&
        hasMacroGlobe)
    {
        return StudioViewportPresentation::MacroGlobe;
    }

    return hasBody
        ? StudioViewportPresentation::BodyPreview
        : StudioViewportPresentation::Blank;
}

// Adds one body/path or physical-terrain-debug composition per owned Studio
// RenderView. Debug textures are persistent derived presentation resources;
// physical terrain authority stays in TerrainDebugPageData's source products.
class StudioViewportRenderer
{
public:
    StudioViewportRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler,
        u32 framesInFlight = 1U);

    [[nodiscard]] std::optional<StudioMacroGlobeDiagnostics>
    MacroGlobeDiagnostics(
        std::string_view viewportId) const noexcept;

    [[nodiscard]] std::optional<
        StudioSurfaceGlobeTransitionDiagnostics>
    SurfaceGlobeTransitionDiagnostics(
        std::string_view viewportId) const noexcept;

    [[nodiscard]] std::optional<
        StudioVisibilityProxyDiagnostics>
    VisibilityProxyDiagnostics(
        std::string_view viewportId) const noexcept;

    [[nodiscard]] std::optional<
        StudioCelestialLightingDiagnostics>
    CelestialLightingDiagnostics(
        std::string_view viewportId) const noexcept;

    [[nodiscard]] std::optional<
        StudioAtmosphereDiagnostics>
    AtmosphereDiagnostics(
        std::string_view viewportId) const noexcept;

    [[nodiscard]] std::optional<
        StudioCloudDiagnostics>
    CloudDiagnostics(
        std::string_view viewportId) const noexcept;

    [[nodiscard]] std::optional<
        StudioOceanDiagnostics>
    OceanDiagnostics(
        std::string_view viewportId) const noexcept;

    [[nodiscard]] std::optional<
        StudioRingDiagnostics>
    RingDiagnostics(
        std::string_view viewportId) const noexcept;

    [[nodiscard]] std::optional<
        StudioStellarDiagnostics>
    StellarDiagnostics(
        std::string_view viewportId) const noexcept;

    [[nodiscard]] std::optional<
        StudioGiantDiagnostics>
    GiantDiagnostics(
        std::string_view viewportId) const noexcept;

    void SetColorLut(
        post_process::ColorLutData lut);

    void SetColorLutSettings(
        post_process::ColorLutSettings settings) noexcept;

    void SetDisplayResolveSettings(
        post_process::DisplayResolveSettings settings) noexcept;

    [[nodiscard]] std::vector<StudioRenderedView> Compose(
        render_graph::RenderGraph& graph,
        StudioRenderViewSet& views,
        studio_session::StudioSession& session,
        studio_session::StudioRuntimeBinding& runtime,
        const studio_session::StudioRuntimeSnapshot& snapshot,
        time::SimulationTime atTime = {},
        bool drawPathDebug = true,
        u32 frameIndex = 0U);

private:
    [[nodiscard]] celestial_globe::GpuMacroGlobeProduct*
    EnsureMacroGlobePresentation(
        std::string_view viewportId,
        studio_session::StudioSession& session,
        universe::BodyId body,
        const universe::BodyShape& shape,
        const terrain::TerrainSource& terrainSource);

    struct DebugPresentation
    {
        std::unique_ptr<terrain_debug::TerrainDebugTexture> texture;
        std::shared_ptr<const terrain_debug::TerrainDebugPageData> source;
        terrain_debug::TerrainDebugField field{
            terrain_debug::TerrainDebugField::Uplift};
        u64 seamFingerprint{0};
    };

    struct CloudPresentation
    {
        universe::BodyId body{};
        u64 fingerprint{0};
        std::unique_ptr<
            celestial_clouds::CloudFieldProduct>
            field;
        std::unique_ptr<
            celestial_clouds::GpuCloudFieldProduct>
            gpu;
    };

    struct GiantPresentation
    {
        universe::BodyId body{};
        u64 fingerprint{0};
        celestial_far_render::AppearanceSummary
            summary{};
        std::unique_ptr<
            celestial_appearance::PlanetaryAppearanceProduct>
            appearance;
        std::unique_ptr<
            celestial_appearance::GpuPlanetaryAppearanceProduct>
            gpuAppearance;
    };

    struct RingPresentation
    {
        universe::BodyId body{};
        u64 fingerprint{0};
        f64 referenceRadiusMeters{1.0};
        std::unique_ptr<
            celestial_rings::GpuRingMeshProduct>
            nearMesh;
        std::unique_ptr<
            celestial_rings::GpuRingMeshProduct>
            farMesh;
        celestial_rings::FarRingProfile
            farProfile{};
    };

    struct AtmospherePresentation
    {
        universe::BodyId body{};
        u64 staticFingerprint{0};
        u64 skyFingerprint{0};
        celestial_atmosphere::AtmosphereParameters
            parameters{};
        std::unique_ptr<
            celestial_atmosphere::AtmosphereStaticLuts>
            staticLuts;
        std::unique_ptr<
            celestial_atmosphere::AtmosphereSkyView>
            skyView;
        std::unique_ptr<
            celestial_atmosphere::GpuAtmosphereLuts>
            gpu;
    };

    struct MacroGlobePresentation
    {
        universe::BodyId body{};
        u64 sourceRevision{0};
        u64 fingerprint{0};
        u64 baseAppearanceFingerprint{0};
        u64 appearanceFingerprint{0};
        u64 cloudFingerprint{0};
        u64 oceanFingerprint{0};
        u64 cachedDiscFingerprint{0};
        u32 appearanceTexels{0};
        celestial_far_render::AppearanceSummary
            appearanceSummary{};
        std::unique_ptr<
            celestial_appearance::GpuPlanetaryAppearanceProduct>
            appearanceProduct;
        std::unique_ptr<
            celestial_far_render::GpuCachedDiscProduct>
            cachedDisc;
        std::unique_ptr<celestial_globe::GpuMacroGlobeProduct> product;
    };

    struct VisibilityProxyPresentation
    {
        universe::BodyId body{};
        frames::FrameId frame{};
        u64 semanticRevision{0U};
        bool hasDynamic{false};
        lighting::SoftwareProxyScene scene;
        std::unique_ptr<
            lighting::SoftwareProxyVisibilityProvider>
            provider;
        std::unique_ptr<
            lighting::HardwareRayQueryVisibilityBatch>
            hardware;
    };

    struct TerrainPresentation
    {
        u64 universeGeneration{0U};
        u64 terrainSourceRevision{0U};
        u64 runtimeGeneration{0U};
        universe::BodyId body{};
        world::PlanetId planet{};
        terrain_view::ClipmapConfig clipmap{};
        world::WorldPosition observer{};

        std::unique_ptr<terrain_gpu::GpuFieldGenerator>
            fieldGenerator;
        std::unique_ptr<terrain_render::TerrainPreviewRenderer>
            renderer;
    };

    rhi::Device* device_{nullptr};
    const shader::Compiler* compiler_{nullptr};
    u32 framesInFlight_{1U};
    editor_ui::BodyPreviewRenderer bodyRenderer_;
    celestial_globe::MacroGlobeRenderer macroGlobeRenderer_;
    celestial_far_render::FarBodyRenderer farBodyRenderer_;
    celestial_rings::RingRenderer ringRenderer_;
    editor_ui::PathPreviewRenderer pathRenderer_;
    render_view::CompositeRenderer debugComposite_;
    lighting::DirectLightingRenderer directLightingRenderer_;
    lighting::SurfaceDebugRenderer surfaceDebugRenderer_;
    post_process::DisplayResolveRenderer displayResolveRenderer_;
    post_process::ColorLutRenderer colorLutRenderer_;
    std::unique_ptr<post_process::GpuColorLut> colorLut_;
    post_process::ColorLutSettings colorLutSettings_{};
    post_process::DisplayResolveSettings displayResolveSettings_{};
    celestial_representation::RepresentationTracker
        representationTracker_;

    std::map<
        std::string,
        DebugPresentation,
        std::less<>>
        debugPresentations_;

    std::map<
        std::string,
        StudioGiantDiagnostics,
        std::less<>>
        giantDiagnostics_;

    std::map<
        std::string,
        GiantPresentation,
        std::less<>>
        giantPresentations_;

    std::map<
        std::string,
        StudioStellarDiagnostics,
        std::less<>>
        stellarDiagnostics_;

    std::map<
        std::string,
        StudioRingDiagnostics,
        std::less<>>
        ringDiagnostics_;

    std::map<
        std::string,
        RingPresentation,
        std::less<>>
        ringPresentations_;

    std::map<
        std::string,
        StudioOceanDiagnostics,
        std::less<>>
        oceanDiagnostics_;

    std::map<
        std::string,
        StudioCloudDiagnostics,
        std::less<>>
        cloudDiagnostics_;

    std::map<
        std::string,
        CloudPresentation,
        std::less<>>
        cloudPresentations_;

    std::map<
        std::string,
        StudioAtmosphereDiagnostics,
        std::less<>>
        atmosphereDiagnostics_;

    std::map<
        std::string,
        AtmospherePresentation,
        std::less<>>
        atmospherePresentations_;

    std::map<
        std::string,
        StudioCelestialLightingDiagnostics,
        std::less<>>
        lightingDiagnostics_;

    std::map<
        std::string,
        StudioSurfaceGlobeTransitionDiagnostics,
        std::less<>>
        transitionDiagnostics_;

    std::map<
        std::string,
        StudioVisibilityProxyDiagnostics,
        std::less<>>
        visibilityProxyDiagnostics_;

    std::map<
        std::string,
        VisibilityProxyPresentation,
        std::less<>>
        visibilityProxyPresentations_;

    std::map<
        std::string,
        MacroGlobePresentation,
        std::less<>>
        macroGlobePresentations_;

    std::map<
        std::string,
        TerrainPresentation,
        std::less<>>
        terrainPresentations_;
};
} // namespace orbit::studio_ui
