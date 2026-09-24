#include <orbit/studio_ui/StudioViewportRenderer.hpp>
#include <orbit/studio_ui/LightingInteractionState.hpp>
#include <orbit/content/ContentService.hpp>
#include <orbit/world_model/MaterialAssignmentBinding.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#define Compose ComposeBase
#include "StudioViewportRendererBase.cpp"
#undef Compose

namespace orbit::studio_ui
{
namespace
{
void AddBoxLines(
    std::vector<editor_ui::PreviewLine>& lines,
    const math::Float3 center,
    const math::Float3 halfExtent,
    const math::Float4 color)
{
    const std::array<math::Float3, 8U> p{{
        center + math::Float3{-halfExtent.x,-halfExtent.y,-halfExtent.z},
        center + math::Float3{ halfExtent.x,-halfExtent.y,-halfExtent.z},
        center + math::Float3{ halfExtent.x, halfExtent.y,-halfExtent.z},
        center + math::Float3{-halfExtent.x, halfExtent.y,-halfExtent.z},
        center + math::Float3{-halfExtent.x,-halfExtent.y, halfExtent.z},
        center + math::Float3{ halfExtent.x,-halfExtent.y, halfExtent.z},
        center + math::Float3{ halfExtent.x, halfExtent.y, halfExtent.z},
        center + math::Float3{-halfExtent.x, halfExtent.y, halfExtent.z}
    }};

    constexpr std::array<std::array<u32, 2U>, 12U> edges{{
        {{0U,1U}},{{1U,2U}},{{2U,3U}},{{3U,0U}},
        {{4U,5U}},{{5U,6U}},{{6U,7U}},{{7U,4U}},
        {{0U,4U}},{{1U,5U}},{{2U,6U}},{{3U,7U}}
    }};

    for (const auto& edge : edges)
    {
        lines.push_back({
            .start = p[edge[0U]],
            .end = p[edge[1U]],
            .color = color
        });
    }
}

void UpdateInteractionDiagnostics(
    StudioViewportRenderer& renderer,
    studio_session::StudioSession& session,
    content::ContentService* content)
{
    auto diagnostics = StudioLightingInteractionDiagnostics{};

    const auto emissive = renderer.EmissiveGiDiagnostics("studio.primary");
    if (emissive.has_value())
    {
        diagnostics.trackedEmissiveSources = emissive->trackedSources;
        diagnostics.invalidationEventsThisFrame =
            emissive->invalidationEventsThisFrame;
        diagnostics.dirtyRadianceCells = emissive->dirtyRadianceCells;
        diagnostics.scheduledRadianceUpdates =
            emissive->scheduledRadianceUpdates;
    }

    const auto visibility = renderer.VisibilityProxyDiagnostics("studio.primary");
    if (visibility.has_value())
    {
        diagnostics.hardwareRayQuerySupported =
            visibility->hardwareRayQuerySupported;
        diagnostics.hardwareRayQueryReady =
            visibility->hardwareRayQueryReady;
        diagnostics.hardwarePrimitiveCount =
            visibility->hardwarePrimitiveCount;
    }

    if (session.World().HasWorld() &&
        session.World().Selection().Ordered().size() == 1U)
    {
        const auto selected =
            session.World().Selection().Ordered().front();
        diagnostics.hasSelection = true;
        diagnostics.selectedObject = selected.ToString();

        if (content != nullptr)
        {
            const auto assignments =
                world_model::ResolveMaterialAssignments(
                    session.World().Objects(),
                    selected);

            for (const auto& assignment : assignments)
            {
                if (assignment.owner != selected)
                {
                    continue;
                }

                const content::AssetRecord* asset =
                    content->FindByPath(assignment.assetId);

                if (asset == nullptr)
                {
                    if (const auto id =
                            content::AssetId::Parse(assignment.assetId);
                        id.has_value())
                    {
                        asset = content->Find(*id);
                    }
                }

                if (asset == nullptr)
                {
                    continue;
                }

                try
                {
                    const auto emission =
                        content->ResolveMaterialEmission(asset->id);
                    diagnostics.emissionLuminanceNits =
                        std::max(
                            diagnostics.emissionLuminanceNits,
                            emission.luminanceNits);
                    diagnostics.emissionGiScale =
                        std::max(
                            diagnostics.emissionGiScale,
                            emission.giScale);
                    diagnostics.emissionGiEnabled =
                        diagnostics.emissionGiEnabled ||
                        emission.contributesToGi;
                    diagnostics.selectedEmissive =
                        diagnostics.selectedEmissive ||
                        emission.luminanceNits > 0.0;
                }
                catch (...)
                {
                    // Diagnostics never change authoring/runtime authority.
                }
            }
        }
    }

    StudioLightingInteractionState() =
        std::move(diagnostics);
}
} // namespace

std::vector<StudioRenderedView>
StudioViewportRenderer::Compose(
    render_graph::RenderGraph& graph,
    StudioRenderViewSet& views,
    studio_session::StudioSession& session,
    studio_session::StudioRuntimeBinding& runtime,
    const studio_session::StudioRuntimeSnapshot& snapshot,
    const time::SimulationTime atTime,
    const bool drawPathDebug,
    const u32 frameIndex,
    const lighting::LightingWorkPlan& lightingPlan,
    lighting::LightingTimestampRecorder* const lightingTimestamps)
{
    auto rendered = ComposeBase(
        graph,
        views,
        session,
        runtime,
        snapshot,
        atTime,
        drawPathDebug,
        frameIndex,
        lightingPlan,
        lightingTimestamps);

    UpdateInteractionDiagnostics(
        *this,
        session,
        content_);

    const auto overlay = StudioLightingOverlays();
    if (!overlay.giUpdateCells &&
        !overlay.radianceCacheRegions &&
        !overlay.reflectionInspection &&
        !overlay.emissiveInfluence)
    {
        return rendered;
    }

    auto* view = views.Find("studio.primary");
    const auto gather = finalGatherPresentations_.find("studio.primary");

    if (view == nullptr ||
        gather == finalGatherPresentations_.end() ||
        gather->second.radianceResidency == nullptr)
    {
        return rendered;
    }

    const auto renderedView =
        std::find_if(
            rendered.begin(),
            rendered.end(),
            [](const StudioRenderedView& item)
            {
                return item.id == "studio.primary";
            });

    if (renderedView == rendered.end())
    {
        return rendered;
    }

    auto lines =
        std::make_shared<std::vector<editor_ui::PreviewLine>>();

    const auto& camera = view->Camera();
    const auto& lightingView = view->Lighting();
    const auto& residency = *gather->second.radianceResidency;
    const auto& config = residency.Config();

    if (overlay.radianceCacheRegions)
    {
        const u32 levelCount =
            std::min(
                config.levelCount,
                std::max(overlay.cacheLevels, 1U));

        for (u32 level = 0U; level < levelCount; ++level)
        {
            const auto key =
                lighting::RadianceCellForPoint(
                    camera.localPositionMeters,
                    config,
                    level,
                    lightingView);
            const auto centerWorld =
                lighting::RadianceCellCenterInFrame(
                    key,
                    config);
            const f32 half =
                static_cast<f32>(
                    lighting::RadianceCellSizeMeters(config, level) *
                    static_cast<f64>(config.cellsPerAxis) * 0.5);
            const math::Float3 center{
                static_cast<f32>(centerWorld.x - camera.localPositionMeters.x),
                static_cast<f32>(centerWorld.y - camera.localPositionMeters.y),
                static_cast<f32>(centerWorld.z - camera.localPositionMeters.z)
            };
            const f32 t =
                config.levelCount > 1U
                    ? static_cast<f32>(level) /
                          static_cast<f32>(config.levelCount - 1U)
                    : 0.0F;
            AddBoxLines(
                *lines,
                center,
                {half, half, half},
                {0.20F + 0.45F * t, 0.82F, 1.0F - 0.35F * t, 0.82F});
        }
    }

    if (overlay.giUpdateCells || overlay.emissiveInfluence)
    {
        const auto updates =
            residency.BuildUpdateList(
                camera.localPositionMeters,
                std::max(overlay.maximumGiCells, 1U));

        for (const auto& update : updates)
        {
            const auto centerWorld =
                lighting::RadianceCellCenterInFrame(
                    update.key,
                    config);
            const f32 half =
                static_cast<f32>(
                    lighting::RadianceCellSizeMeters(
                        config,
                        update.key.level) *
                    0.42);
            const math::Float3 center{
                static_cast<f32>(centerWorld.x - camera.localPositionMeters.x),
                static_cast<f32>(centerWorld.y - camera.localPositionMeters.y),
                static_cast<f32>(centerWorld.z - camera.localPositionMeters.z)
            };
            const bool emissivePriority =
                update.priority > 1.5F;
            const math::Float4 color =
                overlay.emissiveInfluence && emissivePriority
                    ? math::Float4{1.0F, 0.28F, 0.12F, 0.95F}
                    : math::Float4{0.35F, 1.0F, 0.35F, 0.86F};
            AddBoxLines(
                *lines,
                center,
                {half, half, half},
                color);
        }
    }

    if (overlay.reflectionInspection)
    {
        auto forward = camera.forward;
        if (math::LengthSquared(forward) <= 1.0e-8F)
        {
            forward = {0.0F, 0.0F, 1.0F};
        }
        forward = math::Normalize(forward);
        auto up = camera.up;
        if (math::LengthSquared(up) <= 1.0e-8F)
        {
            up = {0.0F, 1.0F, 0.0F};
        }
        up = math::Normalize(up);
        auto right = math::Cross(forward, up);
        if (math::LengthSquared(right) <= 1.0e-8F)
        {
            right = {1.0F, 0.0F, 0.0F};
        }
        right = math::Normalize(right);

        const math::Float3 origin = forward * 0.25F;
        const math::Float3 center = forward * 5.0F;
        constexpr math::Float4 reflectionColor{0.92F, 0.42F, 1.0F, 0.92F};
        lines->push_back({.start=origin,.end=center,.color=reflectionColor});
        lines->push_back({.start=origin,.end=center + right * 1.5F,.color=reflectionColor});
        lines->push_back({.start=origin,.end=center - right * 1.5F,.color=reflectionColor});
        lines->push_back({.start=origin,.end=center + up * 1.5F,.color=reflectionColor});
        lines->push_back({.start=origin,.end=center - up * 1.5F,.color=reflectionColor});
    }

    if (lines->empty())
    {
        return rendered;
    }

    const auto target = renderedView->targets.display;
    const u32 width = view->Width();
    const u32 height = view->Height();
    const auto cameraCopy = camera;

    graph.AddPass(
        "Studio.M41.LightingInteractionOverlay",
        {{
            .texture = target,
            .state = rhi::ResourceState::RenderTarget,
            .access = render_graph::Access::Write
        }},
        [this, lines, target, width, height, cameraCopy](
            rhi::CommandList& commands,
            const render_graph::Resources& resources)
        {
            pathRenderer_.DrawCameraRelativeLines(
                commands,
                resources.Texture(target),
                width,
                height,
                cameraCopy,
                std::span<const editor_ui::PreviewLine>(*lines));
        });

    return rendered;
}
} // namespace orbit::studio_ui
