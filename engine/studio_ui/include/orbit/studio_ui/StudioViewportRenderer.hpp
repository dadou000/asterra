#pragma once

#include <orbit/editor_ui/BodyPreviewRenderer.hpp>
#include <orbit/editor_ui/PathPreviewRenderer.hpp>
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

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace orbit::studio_ui
{
struct StudioRenderedView
{
    std::string id;
    render_view::ImportedTargets targets{};
    bool targeted{false};
};

enum class StudioViewportPresentation : u8
{
    Blank,
    BodyPreview,
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
    struct DebugPresentation
    {
        std::unique_ptr<terrain_debug::TerrainDebugTexture> texture;
        std::shared_ptr<const terrain_debug::TerrainDebugPageData> source;
        terrain_debug::TerrainDebugField field{
            terrain_debug::TerrainDebugField::Uplift};
        u64 seamFingerprint{0};
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
    editor_ui::PathPreviewRenderer pathRenderer_;
    render_view::CompositeRenderer debugComposite_;

    std::map<
        std::string,
        DebugPresentation,
        std::less<>>
        debugPresentations_;

    std::map<
        std::string,
        TerrainPresentation,
        std::less<>>
        terrainPresentations_;
};
} // namespace orbit::studio_ui
