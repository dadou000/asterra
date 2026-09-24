#include <orbit/studio_ui/StudioViewportRenderer.hpp>
#include <orbit/studio_ui/LightingInteractionState.hpp>
#include <orbit/studio_ui/LightingSelectionInspection.hpp>
#include <orbit/studio_ui/StudioLightingOverlayGeometry.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#define Compose ComposeBase
#include "StudioViewportRendererBase.cpp"
#undef Compose

namespace orbit::studio_ui
{
namespace
{
[[nodiscard]] std::vector<lighting::DynamicEmissiveSourceState>
BuildM41EmissiveOverlaySources(
    const scene::ObjectStore& objects)
{
    const auto emissiveVolumes =
        ResolveAuthoredEmissiveVolumes(objects);

    std::vector<lighting::DynamicEmissiveSourceState> result;
    result.reserve(emissiveVolumes.size());

    for (const auto& volume : emissiveVolumes)
    {
        result.push_back({
            .stableId = volume.stableId,
            .contentRevision = 0U,
            .centerInFrameMeters = volume.centerInFrameMeters,
            .sourceRadiusMeters =
                std::max<f64>(volume.radiusMeters, 0.0F),
            .influenceRangeMeters =
                std::max<f64>(volume.influenceRangeMeters, 0.0F)
        });
    }

    return result;
}

void AppendLines(
    std::vector<editor_ui::PreviewLine>& destination,
    std::vector<editor_ui::PreviewLine> source)
{
    destination.insert(
        destination.end(),
        std::make_move_iterator(source.begin()),
        std::make_move_iterator(source.end()));
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
    const auto overlays =
        StudioLightingOverlays();

    std::vector<std::pair<
        render_view::RenderView*,
        lighting::SurfaceDebugMode>>
        restoreSurfaceModes;

    if (overlays.reflectionInspection)
    {
        for (const auto& info : views.Catalog())
        {
            auto* view = views.Find(info.id);
            if (view == nullptr)
            {
                continue;
            }

            const auto previous =
                view->SurfaceDebugMode();

            restoreSurfaceModes.emplace_back(
                view,
                previous);

            // Hybrid/exact reflections consume roughness + metallic as their
            // primary material classification. Reuse the production surface
            // debug presentation for a faithful reflection-input inspection
            // rather than maintaining a separate debug shading model.
            view->SetSurfaceDebugMode(
                lighting::SurfaceDebugMode::NormalMetallic);
        }
    }

    auto rendered =
        ComposeBase(
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

    for (const auto& [view, previous] :
         restoreSurfaceModes)
    {
        if (view != nullptr)
        {
            view->SetSurfaceDebugMode(previous);
        }
    }

    if (content_ != nullptr)
    {
        PublishSelectedLightingAuthority(
            session,
            *content_);
    }

    const bool wantsSpatialOverlay =
        overlays.giUpdateCells ||
        overlays.radianceCacheRegions ||
        overlays.emissiveInfluence;

    if (!wantsSpatialOverlay)
    {
        return rendered;
    }

    const auto emissiveSources =
        overlays.emissiveInfluence &&
                session.World().HasWorld()
            ? BuildM41EmissiveOverlaySources(
                  session.World().Objects())
            : std::vector<
                  lighting::DynamicEmissiveSourceState>{};

    for (const auto& output : rendered)
    {
        auto* view =
            views.Find(output.id);

        if (view == nullptr ||
            !output.targets.display.IsValid())
        {
            continue;
        }

        std::vector<editor_ui::PreviewLine>
            overlayLines;

        const auto gatherFound =
            finalGatherPresentations_.find(
                output.id);

        if (gatherFound !=
                finalGatherPresentations_.end() &&
            gatherFound->second.radianceResidency !=
                nullptr)
        {
            const auto& residency =
                *gatherFound->second.radianceResidency;
            const auto& config =
                residency.Config();
            const auto& lightingView =
                view->Lighting();

            if (overlays.giUpdateCells)
            {
                const auto updates =
                    residency.BuildUpdateList(
                        lightingView.
                            cameraPositionInFrameMeters,
                        overlays.maximumGiCells);

                AppendLines(
                    overlayLines,
                    BuildGiUpdateCellOverlayLines(
                        updates,
                        config,
                        lightingView,
                        overlays.maximumGiCells));
            }

            if (overlays.radianceCacheRegions)
            {
                AppendLines(
                    overlayLines,
                    BuildRadianceCacheRegionOverlayLines(
                        lightingView.
                            cameraPositionInFrameMeters,
                        config,
                        lightingView,
                        overlays.cacheLevels));
            }

            if (overlays.emissiveInfluence &&
                !emissiveSources.empty())
            {
                AppendLines(
                    overlayLines,
                    BuildEmissiveInfluenceOverlayLines(
                        emissiveSources,
                        lightingView));
            }
        }

        if (overlayLines.empty())
        {
            continue;
        }

        auto* display =
            &view->DisplayColor();
        const auto camera =
            view->Camera();
        const u32 width =
            view->Width();
        const u32 height =
            view->Height();

        graph.AddPass(
            "Studio." + output.id +
                ".M41LightingInspection",
            {
                {
                    .texture = output.targets.display,
                    .state =
                        rhi::ResourceState::RenderTarget,
                    .access =
                        render_graph::Access::Write
                }
            },
            [this,
             display,
             width,
             height,
             camera,
             overlayLines =
                 std::move(overlayLines)](
                rhi::CommandList& commands,
                const render_graph::Resources&)
            {
                pathRenderer_.DrawCameraRelativeLines(
                    commands,
                    *display,
                    width,
                    height,
                    camera,
                    overlayLines);
            });
    }

    return rendered;
}
} // namespace orbit::studio_ui
