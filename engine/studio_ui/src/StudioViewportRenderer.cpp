#include <orbit/studio_ui/StudioViewportRenderer.hpp>

#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/studio_ui/StudioTerrainOverlayGeometry.hpp>
#include <orbit/world_model/WorldSchemas.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <utility>

namespace orbit::studio_ui
{
namespace
{
[[nodiscard]] bool SameClipmapConfig(
    const terrain_view::ClipmapConfig& a,
    const terrain_view::ClipmapConfig& b) noexcept
{
    return
        a.levelCount == b.levelCount &&
        a.gridResolution == b.gridResolution &&
        a.baseSpacingMeters == b.baseSpacingMeters &&
        a.levelScale == b.levelScale &&
        a.overlapCells == b.overlapCells;
}

[[nodiscard]] std::optional<StudioTerrainAuthoringOverlay>
SelectedTerrainConstraintOverlay(
    studio_session::StudioSession& session,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime)
{
    if (!session.World().HasWorld())
    {
        return std::nullopt;
    }

    auto& world = session.World();
    const auto& selected = world.Selection().Ordered();

    if (selected.size() != 1U)
    {
        return std::nullopt;
    }

    scene::ObjectId constraintId = selected.front();
    const auto selectedRecord = world.Objects().Find(constraintId);

    if (!selectedRecord.has_value())
    {
        return std::nullopt;
    }

    if (selectedRecord->type ==
        world_model::kTerrainConstraintControlPointType)
    {
        if (!selectedRecord->parent.has_value())
        {
            return std::nullopt;
        }

        constraintId = *selectedRecord->parent;
    }

    const auto constraintRecord =
        world.Objects().Find(constraintId);

    if (!constraintRecord.has_value() ||
        constraintRecord->type !=
            world_model::kTerrainConstraintType)
    {
        return std::nullopt;
    }

    editor_model::SurfaceAuthoringModel model(
        world.Objects(),
        world.Commands(),
        world.Selection());

    const auto body =
        model.SelectedRockyBody();

    if (!body.has_value() ||
        body->terrain != runtime.terrainObject)
    {
        return std::nullopt;
    }

    const auto constraints =
        model.TerrainConstraints(body->terrain);

    const auto found =
        std::find_if(
            constraints.begin(),
            constraints.end(),
            [&](const editor_model::SurfaceTerrainConstraintDetail& item)
            {
                return item.id == constraintId;
            });

    if (found == constraints.end())
    {
        return std::nullopt;
    }

    StudioTerrainAuthoringOverlay overlay{
        .body = runtime.body,
        .kind =
            found->shape ==
                    editor_model::SurfaceTerrainConstraintShape::Spline
                ? StudioTerrainOverlayKind::Spline
                : StudioTerrainOverlayKind::Brush,
        .influenceRadiusMeters =
            found->shape ==
                    editor_model::SurfaceTerrainConstraintShape::Spline
                ? found->halfWidthMeters + found->falloffMeters
                : found->outerRadiusMeters
    };

    if (found->shape ==
        editor_model::SurfaceTerrainConstraintShape::Spline)
    {
        overlay.controlUnitDirections =
            found->controlUnitDirections;
    }
    else
    {
        overlay.controlUnitDirections.push_back(
            found->centerUnitDirection);
    }

    return overlay;
}

[[nodiscard]] terrain_render::TerrainPreviewCamera
TerrainCameraFromBodyCamera(
    const render_view::CameraState& camera,
    const world::WorldPosition& observer)
{
    const auto observerDirection =
        math::Normalize(observer.meters);

    const auto frame =
        world::MakeSurfaceFrame(
            observerDirection);

    const math::Double3 forward{
        static_cast<f64>(camera.forward.x),
        static_cast<f64>(camera.forward.y),
        static_cast<f64>(camera.forward.z)
    };

    const math::Double3 up{
        static_cast<f64>(camera.up.x),
        static_cast<f64>(camera.up.y),
        static_cast<f64>(camera.up.z)
    };

    auto localForward =
        math::Float3{
            static_cast<f32>(
                math::Dot(
                    forward,
                    frame.east)),
            static_cast<f32>(
                math::Dot(
                    forward,
                    frame.up)),
            static_cast<f32>(
                math::Dot(
                    forward,
                    frame.north))
        };

    auto localUp =
        math::Float3{
            static_cast<f32>(
                math::Dot(
                    up,
                    frame.east)),
            static_cast<f32>(
                math::Dot(
                    up,
                    frame.up)),
            static_cast<f32>(
                math::Dot(
                    up,
                    frame.north))
        };

    if (math::LengthSquared(localForward) <=
        1.0e-8F)
    {
        localForward = {
            0.0F,
            -0.28F,
            1.0F
        };
    }

    if (math::LengthSquared(localUp) <=
        1.0e-8F)
    {
        localUp = {
            0.0F,
            1.0F,
            0.0F
        };
    }

    return {
        .forward =
            math::Normalize(
                localForward),
        .up =
            math::Normalize(
                localUp)
    };
}
} // namespace

StudioViewportRenderer::StudioViewportRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler,
    const u32 framesInFlight)
    : device_(&device),
      compiler_(&compiler),
      framesInFlight_(framesInFlight),
      bodyRenderer_(device, compiler),
      pathRenderer_(device, compiler),
      debugComposite_(device, compiler)
{
    if (framesInFlight_ == 0U)
    {
        throw std::invalid_argument(
            "Studio terrain rendering requires at least one frame-in-flight slot.");
    }
}

std::vector<StudioRenderedView>
StudioViewportRenderer::Compose(
    render_graph::RenderGraph& graph,
    StudioRenderViewSet& views,
    studio_session::StudioSession& session,
    studio_session::StudioRuntimeBinding& runtime,
    const studio_session::StudioRuntimeSnapshot& snapshot,
    const time::SimulationTime atTime,
    const bool drawPathDebug,
    const u32 frameIndex)
{
    static_cast<void>(views.Refresh(snapshot));

    const universe::BodyRegistry* bodies = nullptr;
    const frames::FrameGraph* frames = nullptr;
    std::vector<const path_geometry::PathDerivedProduct*> products;

    if (snapshot.hasWorld)
    {
        bodies = &runtime.Bodies(snapshot);
        frames = &runtime.Frames(snapshot);
        products = runtime.PathProducts(snapshot).Products();
    }

    std::vector<StudioRenderedView> rendered;
    const auto catalog = views.Catalog();
    rendered.reserve(catalog.size());

    for (const auto& info : catalog)
    {
        auto* view = views.Find(info.id);

        if (view == nullptr)
        {
            throw std::logic_error(
                "Studio RenderView catalog contains a missing view.");
        }

        const std::string prefix =
            "StudioViewport." + info.id;
        const auto targets =
            view->Import(
                graph,
                prefix.c_str());

        const auto* logicalTarget =
            session.Viewports().Find(info.id);

        if (logicalTarget == nullptr)
        {
            throw std::logic_error(
                "Studio RenderView has no logical viewport target.");
        }

        std::optional<universe::BodyShape> shape;

        if (logicalTarget->target.has_value())
        {
            if (!snapshot.hasWorld || bodies == nullptr ||
                logicalTarget->target->universeGeneration !=
                    snapshot.universeGeneration)
            {
                throw std::logic_error(
                    "Studio viewport render target is stale for the current universe generation.");
            }

            const auto* body =
                bodies->FindBody(
                    logicalTarget->target->body);

            if (body == nullptr)
            {
                throw std::logic_error(
                    "Studio viewport target body is missing from the current BodyRegistry.");
            }

            shape = body->shape;
        }

        const auto terrainRuntime =
            session.TerrainRuntime().
                Capture(
                    info.id);

        if (terrainRuntime.has_value() &&
            !session.TerrainRuntime().
                IsCurrent(
                    *terrainRuntime))
        {
            throw std::logic_error(
                "Studio terrain viewport runtime is stale for the current session generation.");
        }

        const auto liveDebugPage =
            views.LiveDebugPage(info.id);
        const bool hasDebugField =
            liveDebugPage != nullptr &&
            liveDebugPage->Has(
                info.debugField);

        const auto presentation =
            SelectStudioViewportPresentation(
                logicalTarget->mode,
                shape.has_value(),
                terrainRuntime.has_value(),
                liveDebugPage != nullptr,
                hasDebugField);

        const u32 width = view->Width();
        const u32 height = view->Height();
        auto* color = &view->Color();

        switch (presentation)
        {
        case StudioViewportPresentation::ProductionTerrain:
        {
            if (device_ == nullptr ||
                compiler_ == nullptr ||
                !terrainRuntime.has_value())
            {
                throw std::logic_error(
                    "Studio production-terrain presentation lost its device, compiler or runtime binding.");
            }

            const auto& source =
                session.TerrainRuntime().
                    TerrainSource(
                        *terrainRuntime);

            const auto* analytic =
                dynamic_cast<
                    const terrain::
                        AnalyticTerrainSource*>(
                            &source);

            if (analytic == nullptr)
            {
                throw std::logic_error(
                    "Studio production terrain currently requires the composed AnalyticTerrainSource used by the shared GPU field generator.");
            }

            auto& terrain =
                terrainPresentations_[
                    info.id];

            const bool recreate =
                terrain.renderer == nullptr ||
                terrain.fieldGenerator == nullptr ||
                terrain.universeGeneration !=
                    terrainRuntime->
                        universeGeneration ||
                terrain.body !=
                    terrainRuntime->body ||
                terrain.planet !=
                    terrainRuntime->planet.id ||
                terrain.terrainSourceRevision !=
                    terrainRuntime->
                        terrainSourceRevision ||
                !SameClipmapConfig(
                    terrain.clipmap,
                    terrainRuntime->clipmap);

            if (recreate)
            {
                terrain_render::
                    TerrainPreviewConfig
                    config{};

                config.clipmap =
                    terrainRuntime->clipmap;
                config.adaptiveCoverage.enabled =
                    false;
                config.nearPlaneMeters =
                    std::max(
                        view->Camera().
                            nearPlaneMeters,
                        0.01F);
                config.farPlaneMeters =
                    std::max(
                        view->Camera().
                            farPlaneMeters,
                        config.nearPlaneMeters *
                            100.0F);
                config.framesInFlight =
                    framesInFlight_;

                terrain.fieldGenerator =
                    std::make_unique<
                        terrain_gpu::
                            GpuFieldGenerator>(
                                *device_,
                                *compiler_,
                                terrainRuntime->
                                    planet,
                                *analytic);

                terrain.renderer =
                    std::make_unique<
                        terrain_render::
                            TerrainPreviewRenderer>(
                                *device_,
                                *compiler_,
                                terrainRuntime->
                                    planet,
                                *terrain.
                                    fieldGenerator,
                                terrainRuntime->
                                    observer,
                                config);

                terrain.universeGeneration =
                    terrainRuntime->
                        universeGeneration;
                terrain.body =
                    terrainRuntime->body;
                terrain.planet =
                    terrainRuntime->planet.id;
                terrain.terrainSourceRevision =
                    terrainRuntime->
                        terrainSourceRevision;
                terrain.clipmap =
                    terrainRuntime->clipmap;
                terrain.observer =
                    terrainRuntime->observer;
            }
            else if (
                terrain.observer.meters !=
                    terrainRuntime->
                        observer.meters)
            {
                terrain.renderer->
                    UpdateObserver(
                        terrainRuntime->
                            observer);

                terrain.observer =
                    terrainRuntime->observer;
            }

            terrain.runtimeGeneration =
                terrainRuntime->
                    runtimeGeneration;

            const auto camera =
                TerrainCameraFromBodyCamera(
                    view->Camera(),
                    terrainRuntime->observer);

            auto* terrainRenderer =
                terrain.renderer.get();
            auto* depth =
                &view->Depth();

            graph.AddPass(
                prefix + ".ProductionTerrain",
                {
                    {
                        .texture = targets.color,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    },
                    {
                        .texture = targets.depth,
                        .state = rhi::ResourceState::DepthWrite,
                        .access = render_graph::Access::Write
                    }
                },
                [color,
                 depth,
                 width,
                 height,
                 terrainRenderer,
                 camera,
                 frameIndex,
                 framesInFlight =
                    framesInFlight_](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    commands.ClearColorTarget(
                        *color,
                        {
                            .red = 0.008F,
                            .green = 0.012F,
                            .blue = 0.020F,
                            .alpha = 1.0F
                        });

                    commands.ClearDepthTarget(
                        *depth,
                        0.0F);

                    commands.SetRenderTargets(
                        *color,
                        *depth);

                    terrainRenderer->Draw(
                        commands,
                        frameIndex %
                            framesInFlight,
                        width,
                        height,
                        camera);
                });
            break;
        }

        case StudioViewportPresentation::TerrainDebug:
        {
            terrainPresentations_.erase(
                info.id);

            if (device_ == nullptr ||
                liveDebugPage == nullptr)
            {
                throw std::logic_error(
                    "Studio terrain-debug presentation lost its device or live page.");
            }

            auto& debug =
                debugPresentations_[info.id];

            if (debug.texture == nullptr ||
                debug.texture->Width() !=
                    liveDebugPage->Width() ||
                debug.texture->Height() !=
                    liveDebugPage->Height())
            {
                debug.texture =
                    std::make_unique<
                        terrain_debug::TerrainDebugTexture>(
                            *device_,
                            liveDebugPage->Width(),
                            liveDebugPage->Height());
                debug.source.reset();
            }

            const auto seams =
                terrain_debug::InspectTerrainDebugSeams(
                    *liveDebugPage,
                    info.debugField,
                    session.TerrainDebugPages());

            const u64 seamFingerprint =
                terrain_debug::
                    TerrainDebugSeamOverlayFingerprint(
                        seams);

            const bool needsUpload =
                debug.source != liveDebugPage ||
                debug.field != info.debugField ||
                debug.seamFingerprint !=
                    seamFingerprint ||
                !debug.texture->HasContent();

            auto* debugTexture =
                debug.texture.get();
            const auto field =
                info.debugField;

            graph.AddPass(
                prefix + ".TerrainDebug",
                {
                    {
                        .texture = targets.color,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    }
                },
                [this,
                 color,
                 width,
                 height,
                 debugTexture,
                 liveDebugPage,
                 field,
                 seams,
                 needsUpload](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    if (needsUpload)
                    {
                        debugTexture->Upload(
                            commands,
                            liveDebugPage->View(field),
                            seams);
                    }

                    debugComposite_.Draw(
                        commands,
                        debugTexture->Texture(),
                        *color,
                        width,
                        height);
                });

            debug.source = liveDebugPage;
            debug.field = field;
            debug.seamFingerprint =
                seamFingerprint;
            break;
        }

        case StudioViewportPresentation::TerrainDebugUnavailable:
            terrainPresentations_.erase(
                info.id);

            graph.AddPass(
                prefix + ".TerrainDebugUnavailable",
                {
                    {
                        .texture = targets.color,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    }
                },
                [color](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    commands.ClearColorTarget(
                        *color,
                        {
                            .red = 0.055F,
                            .green = 0.018F,
                            .blue = 0.024F,
                            .alpha = 1.0F
                        });
                });
            break;

        case StudioViewportPresentation::BodyPreview:
        {
            terrainPresentations_.erase(
                info.id);

            const auto camera = view->Camera();
            const auto bodyShape = *shape;

            graph.AddPass(
                prefix + ".Body",
                {
                    {
                        .texture = targets.color,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    }
                },
                [this,
                 color,
                 width,
                 height,
                 camera,
                 bodyShape](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    bodyRenderer_.Draw(
                        commands,
                        *color,
                        width,
                        height,
                        bodyShape,
                        camera);
                });

            if (frames != nullptr &&
                !products.empty())
            {
                const auto* frameGraph = frames;
                const auto pathProducts = products;

                graph.AddPass(
                    prefix + ".Paths",
                    {
                        {
                            .texture = targets.color,
                            .state = rhi::ResourceState::RenderTarget,
                            .access = render_graph::Access::Write
                        }
                    },
                    [this,
                     color,
                     width,
                     height,
                     camera,
                     frameGraph,
                     pathProducts,
                     atTime,
                     drawPathDebug](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        pathRenderer_.Draw(
                            commands,
                            *color,
                            width,
                            height,
                            camera,
                            *frameGraph,
                            atTime,
                            std::span<
                                const path_geometry::PathDerivedProduct* const>(
                                    pathProducts.data(),
                                    pathProducts.size()),
                            drawPathDebug);
                    });
            }
            break;
        }

        case StudioViewportPresentation::Blank:
            terrainPresentations_.erase(
                info.id);

            graph.AddPass(
                prefix + ".Blank",
                {
                    {
                        .texture = targets.color,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    }
                },
                [color](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    commands.ClearColorTarget(
                        *color,
                        {
                            .red = 0.018F,
                            .green = 0.021F,
                            .blue = 0.027F,
                            .alpha = 1.0F
                        });
                });
            break;
        }

        if (terrainRuntime.has_value() &&
            logicalTarget->mode !=
                studio_session::ViewportMode::Debug)
        {
            auto overlay =
                views.TerrainAuthoringOverlay(info.id);

            if (!overlay.has_value())
            {
                overlay =
                    SelectedTerrainConstraintOverlay(
                        session,
                        *terrainRuntime);
            }

            if (overlay.has_value() &&
                overlay->body == terrainRuntime->body)
            {
                const auto& source =
                    session.TerrainRuntime().TerrainSource(
                        *terrainRuntime);

                auto overlayLines =
                    BuildTerrainAuthoringOverlayLines(
                        *overlay,
                        *terrainRuntime,
                        source,
                        view->Camera());

                if (!overlayLines.empty())
                {
                    const auto camera = view->Camera();
                    auto* overlayColor = color;

                    graph.AddPass(
                        prefix + ".TerrainAuthoringOverlay",
                        {
                            {
                                .texture = targets.color,
                                .state = rhi::ResourceState::RenderTarget,
                                .access = render_graph::Access::Write
                            }
                        },
                        [this,
                         overlayColor,
                         width,
                         height,
                         camera,
                         overlayLines = std::move(overlayLines)](
                            rhi::CommandList& commands,
                            const render_graph::Resources&)
                        {
                            pathRenderer_.DrawCameraRelativeLines(
                                commands,
                                *overlayColor,
                                width,
                                height,
                                camera,
                                overlayLines);
                        });
                }
            }
        }

        rendered.push_back({
            .id = info.id,
            .targets = targets,
            .targeted = shape.has_value()
        });
    }

    for (auto iterator =
             terrainPresentations_.begin();
         iterator !=
             terrainPresentations_.end();)
    {
        const bool stillExists =
            std::find_if(
                catalog.begin(),
                catalog.end(),
                [&iterator](
                    const StudioRenderViewInfo& item)
                {
                    return item.id ==
                        iterator->first;
                }) !=
            catalog.end();

        if (!stillExists)
        {
            iterator =
                terrainPresentations_.
                    erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }

    return rendered;
}
} // namespace orbit::studio_ui
