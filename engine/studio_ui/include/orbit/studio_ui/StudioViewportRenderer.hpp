#pragma once

#include <orbit/celestial_appearance/PlanetaryAppearance.hpp>
#include <orbit/celestial_globe/MacroGlobe.hpp>
#include <orbit/celestial_far_render/FarBodyRenderer.hpp>
#include <orbit/celestial_representation/RepresentationTracker.hpp>
#include <orbit/editor_ui/BodyPreviewRenderer.hpp>
#include <orbit/editor_ui/PathPreviewRenderer.hpp>
#include <orbit/post_process/ColorLut.hpp>
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

struct StudioCelestialLightingDiagnostics
{
    universe::BodyId receiver{};
    universe::BodyId emitter{};
    f64 visibleFraction{1.0};
    f64 irradianceWattsPerSquareMeter{0.0};
    u32 contributingOccluders{0};
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
        StudioCelestialLightingDiagnostics>
    CelestialLightingDiagnostics(
        std::string_view viewportId) const noexcept;

    void SetColorLut(
        post_process::ColorLutData lut);

    void SetColorLutSettings(
        post_process::ColorLutSettings settings) noexcept;

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

    struct MacroGlobePresentation
    {
        universe::BodyId body{};
        u64 sourceRevision{0};
        u64 fingerprint{0};
        u64 appearanceFingerprint{0};
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
    editor_ui::PathPreviewRenderer pathRenderer_;
    render_view::CompositeRenderer debugComposite_;
    post_process::ColorLutRenderer colorLutRenderer_;
    std::unique_ptr<post_process::GpuColorLut> colorLut_;
    post_process::ColorLutSettings colorLutSettings_{};
    celestial_representation::RepresentationTracker
        representationTracker_;

    std::map<
        std::string,
        DebugPresentation,
        std::less<>>
        debugPresentations_;

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
