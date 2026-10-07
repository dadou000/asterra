#include "StudioPanels.hpp"

#include "EditorAppSupport.hpp"

#include <orbit/build/BuildService.hpp>
#include <orbit/commands/CommandRegistry.hpp>
#include <orbit/commands/CommandService.hpp>
#include <orbit/content/ContentService.hpp>
#include <orbit/content/RuntimeTexture.hpp>
#include <orbit/content_wic/WicTextureImporter.hpp>
#include <orbit/core/Log.hpp>
#include <orbit/core/ThreadName.hpp>
#include <orbit/dev_server/DevServer.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/BuiltinSchemas.hpp>
#include <orbit/editor_model/CommandSurfaces.hpp>
#include <orbit/editor_model/ExplorerModel.hpp>
#include <orbit/editor_model/InspectorModel.hpp>
#include <orbit/editor_model/OutputLog.hpp>
#include <orbit/editor_model/ShortcutRegistry.hpp>
#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/editor_rpc/EditorRpcService.hpp>
#include <orbit/editor_rpc/ProfilerRpc.hpp>
#include <orbit/editor_ui/BodyPreviewRenderer.hpp>
#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/frames/FrameGraph.hpp>
#include <orbit/jobs/JobSystem.hpp>
#include <orbit/lighting/LightingScheduler.hpp>
#include <orbit/lighting/MaterialEmission.hpp>
#include <orbit/path_geometry/PathDerived.hpp>
#include <orbit/path_geometry/PathSource.hpp>
#include <orbit/path_routing/RouteDomains.hpp>
#include <orbit/path_routing/RoutePlanner.hpp>
#include <orbit/paths/PathNetwork.hpp>
#include <orbit/platform/FileBrowser.hpp>
#include <orbit/platform/FileDialog.hpp>
#include <orbit/platform/Paths.hpp>
#include <orbit/platform_services/PlatformConfig.hpp>
#include <orbit/plugins/PluginManager.hpp>
#include <orbit/profiler/Profiler.hpp>
#include <orbit/render_graph/GpuPassTimer.hpp>
#include <orbit/render_graph/RenderGraph.hpp>
#include <orbit/render_view/Capture.hpp>
#include <orbit/render_view/RenderView.hpp>
#include <orbit/rhi/vulkan/VulkanBackend.hpp>
#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/runtime/RuntimeSession.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/selection/SelectionService.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <orbit/shading/MaterialThumbnailCache.hpp>
#include <orbit/shading/ShaderPreviewRenderer.hpp>
#include <orbit/shading/ShadingCapture.hpp>
#include <orbit/shading/ShadingRpc.hpp>
#include <orbit/shading/ShadingWorkspace.hpp>
#include <orbit/studio_session/StudioRuntimeBinding.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/studio_session/StudioTerrainRoundTripVerifier.hpp>
#include <orbit/studio_session/StudioTerrainValidationScenario.hpp>
#include <orbit/studio_session/StudioWorkspace.hpp>
#include <orbit/studio_ui/CelestialAuthoringUi.hpp>
#include <orbit/studio_ui/DebugViewUi.hpp>
#include <orbit/studio_ui/DisplayDiagnosticsUi.hpp>
#include <orbit/studio_ui/DisplayEyeRpc.hpp>
#include <orbit/studio_ui/ProfilerUi.hpp>
#include <orbit/studio_ui/ProjectAuthoringUi.hpp>
#include <orbit/studio_ui/ProjectSettingsUi.hpp>
#include <orbit/studio_ui/ReportThumbnailCache.hpp>
#include <orbit/studio_ui/ReportsUi.hpp>
#include <orbit/studio_ui/ShadingUi.hpp>
#include <orbit/studio_ui/SimulationControlsUi.hpp>
#include <orbit/studio_ui/StudioFlatMapRpc.hpp>
#include <orbit/studio_ui/StudioRenderViewRpc.hpp>
#include <orbit/studio_ui/StudioRenderViewSet.hpp>
#include <orbit/studio_ui/StudioTextDiagnosticsHud.hpp>
#include <orbit/studio_ui/StudioViewContinuity.hpp>
#include <orbit/studio_ui/StudioViewportPanels.hpp>
#include <orbit/studio_ui/StudioViewportRenderer.hpp>
#include <orbit/studio_ui/SurfaceAuthoringUi.hpp>
#include <orbit/studio_ui/SystemViewUi.hpp>
#include <orbit/studio_ui/ViewportCaptureService.hpp>
#include <orbit/studio_ui/VolumeAuthoringUi.hpp>
#include <orbit/studio_ui/WorldDocumentsUi.hpp>
#include <orbit/universe/BodyRegistry.hpp>
#include <orbit/universe/ReferenceSurface.hpp>
#include <orbit/volume_fields/VolumeFieldStorage.hpp>
#include <orbit/volume_solver/SurfaceVolumeSolver.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/MaterialAssignmentBinding.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace orbit::editor_app
{
using namespace support;

void StudioViewportPanel::Register()
{
    ui.RegisterPanel({
        .id = kViewportPanel,
        .title = "Viewport",
        .defaultOpen = true,
        .defaultDock = orbit::editor_ui::DockRegion::Center,
        .dockOrder = 0,
        .minSize = {.width = 320.0F, .height = 200.0F},
        .draw =
            [this](
                orbit::editor_ui::
                    PanelContext& context)
            {
                if (!worldSession.HasWorld())
                {
                    context.Text("No world is open. Use World Documents to open or create one.");
                    return;
                }

                auto* primaryView =
                    studioViews.Find(
                        "studio.primary");

                if (primaryView == nullptr)
                {
                    context.Text(
                        "Primary Studio RenderView is unavailable.");
                    return;
                }

                const auto toolbar =
                    presentActions(
                        "viewport",
                        orbit::editor_model::
                            CommandSurfaceKind::
                                Toolbar);

                if (context.Button(
                        pathPlacementMode
                            ? "Path Tool: On"
                            : "Path Tool"))
                {
                    pathPlacementMode =
                        !pathPlacementMode;

                    if (!pathPlacementMode)
                    {
                        lastPlacedPathNode.
                            reset();
                    }
                }

                context.SameLine();
                context.Toolbar(toolbar);

                context.SameLine();
                static_cast<void>(
                    context.Checkbox(
                        "Path Debug",
                        pathDebugVisualization));

                context.SameLine();
                if (context.Button(
                        "Focus Body"))
                {
                    static_cast<void>(
                        studioViews.
                            FocusTerrainBody(
                                "studio.primary"));
                }

                context.SameLine();
                if (context.Button("Frame Selected"))
                {
                    static_cast<void>(
                        studioViewportPanels.FrameSelectedObject());
                }

                context.SameLine();
                if (context.Button(
                        "Reset View"))
                {
                    static_cast<void>(
                        studioViews.
                            ResetTerrainView(
                                "studio.primary"));
                }

                context.SameLine();
                if (context.InputDouble(
                        "Nav Speed x",
                        viewportNavigationSpeedScale))
                {
                    if (!(viewportNavigationSpeedScale >
                          0.0))
                    {
                        viewportNavigationSpeedScale =
                            1.0;
                    }

                    viewportNavigationSpeedScale =
                        std::clamp(
                            viewportNavigationSpeedScale,
                            0.01,
                            1'000.0);

                    studioViews.
                        SetNavigationSpeedScale(
                            "studio.primary",
                            viewportNavigationSpeedScale);
                }

                // Camera zoom (also the mouse wheel over the view) and the
                // high-resolution captures. The same operations are
                // view.zoom_* and viewport.capture_* over RPC / MCP.
                {
                    // Own row: the row above ends in a full-width input.
                    if (viewportCapture.Active())
                    {
                        context.MutedText("Capturing...");
                    }
                    else
                    {
                        const auto startCapture =
                            [this](
                                const orbit::studio_ui::
                                    ViewportCaptureService::Kind kind)
                        {
                            try
                            {
                                viewportCapture.Start({.kind = kind});
                            }
                            catch (const std::exception& exception)
                            {
                                orbit::log::Warning(
                                    std::format(
                                        "Viewport capture: {}",
                                        exception.what()));
                            }
                        };

                        if (context.Button(
                                "Screenshot##viewport-shot"))
                        {
                            startCapture(
                                orbit::studio_ui::
                                    ViewportCaptureService::Kind::
                                        Fullscreen);
                        }
                        context.SameLine();
                        if (context.Button(
                                "Ultra 16K##viewport-ultra"))
                        {
                            startCapture(
                                orbit::studio_ui::
                                    ViewportCaptureService::Kind::
                                        Ultra);
                        }
                    }

                    // The folder the screenshots are saved to, in the file browser.
                    context.SameLine();
                    if (context.Button(
                            "Screenshot Files##viewport-shot-files"))
                    {
                        try
                        {
                            static_cast<void>(
                                viewportCapture.OpenFolder());
                        }
                        catch (const std::exception& exception)
                        {
                            orbit::log::Warning(
                                std::format(
                                    "Screenshot folder: {}",
                                    exception.what()));
                        }
                    }

                    context.SameLine();
                    if (context.Button("1x##viewport-zoom-reset"))
                    {
                        studioViews.SetZoom(
                            "studio.primary",
                            1.0);
                    }

                    context.SameLine();
                    double zoom =
                        studioViews.Zoom("studio.primary");
                    if (context.InputDouble(
                            "Zoom x",
                            zoom))
                    {
                        studioViews.SetZoom(
                            "studio.primary",
                            zoom);
                    }
                }

                const auto available =
                    context.ContentAvailable();

                const orbit::u32 width =
                    static_cast<orbit::u32>(
                        std::max(
                            available.width,
                            1.0F));

                const orbit::u32 height =
                    static_cast<orbit::u32>(
                        std::max(
                            available.height -
                                22.0F,
                            1.0F));

                if (!viewportCapture.Active() &&
                    (width !=
                        primaryView->Width() ||
                    height !=
                        primaryView->Height()))
                {
                    pendingPrimaryViewResize =
                        std::pair{width, height};
                }

                auto interaction =
                    context.Image(
                        primaryView->DisplayColor(),
                        {
                            .width =
                                static_cast<
                                    orbit::f32>(
                                        primaryView->
                                            Width()),
                            .height =
                                static_cast<
                                    orbit::f32>(
                                        primaryView->
                                            Height())
                        });

                // Move / Rotate / Scale handles of the selected object.
                // While they own the pointer the press is not a
                // selection or path-placement click.
                if (studioViewportPanels.HandleViewportGizmo(
                        context,
                        "studio.primary"))
                {
                    interaction.clicked = false;
                    interaction.doubleClicked = false;
                }

                if (studioViewportPanels.GizmoDragging())
                {
                    window.SetRelativeMouseMode(true);
                }
                else if (!viewportRightGestureActive &&
                         window.RelativeMouseMode())
                {
                    window.SetRelativeMouseMode(false);
                }

                if (interaction.hovered &&
                    interaction.wheel != 0.0F &&
                    !viewportCapture.Active())
                {
                    studioViews.SetZoom(
                        "studio.primary",
                        studioViews.Zoom("studio.primary") *
                            std::pow(
                                1.15,
                                static_cast<double>(
                                    interaction.wheel)));
                }

                primaryTextHud.Draw(
                    context,
                    studioViews,
                    "studio.primary",
                    interaction);

                bool viewportRadialOpen = false;

                const bool rightMouseDown =
                    window.MouseButtonDown(
                        orbit::platform::
                            MouseButton::Right);

                if (!viewportRightGestureActive &&
                    interaction.hovered &&
                    rightMouseDown)
                {
                    viewportRightGestureActive =
                        true;
                    viewportRightGestureDragged =
                        false;
                    viewportRightGestureDistance =
                        0;

                    window.SetRelativeMouseMode(
                        true);
                }

                if (viewportRightGestureActive &&
                    rightMouseDown)
                {
                    const orbit::i32 deltaX =
                        viewportFrameMouseDelta.x;
                    const orbit::i32 deltaY =
                        viewportFrameMouseDelta.y;

                    viewportRightGestureDistance +=
                        (deltaX < 0
                             ? -deltaX
                             : deltaX) +
                        (deltaY < 0
                             ? -deltaY
                             : deltaY);

                    const orbit::f64 moveRight =
                        (window.KeyDown(
                             orbit::platform::
                                 Key::D)
                             ? 1.0
                             : 0.0) -
                        (window.KeyDown(
                             orbit::platform::
                                 Key::A)
                             ? 1.0
                             : 0.0);

                    const orbit::f64 moveForward =
                        (window.KeyDown(
                             orbit::platform::
                                 Key::W)
                             ? 1.0
                             : 0.0) -
                        (window.KeyDown(
                             orbit::platform::
                                 Key::S)
                             ? 1.0
                             : 0.0);

                    const orbit::f64 moveUp =
                        (window.KeyDown(
                             orbit::platform::
                                 Key::E)
                             ? 1.0
                             : 0.0) -
                        (window.KeyDown(
                             orbit::platform::
                                 Key::Q)
                             ? 1.0
                             : 0.0);

                    if (viewportRightGestureDistance >
                            3 ||
                        moveRight != 0.0 ||
                        moveForward != 0.0 ||
                        moveUp != 0.0)
                    {
                        viewportRightGestureDragged =
                            true;
                    }

                    static_cast<void>(
                        studioViews.
                            NavigateTerrain(
                                "studio.primary",
                                {
                                    .deltaSeconds =
                                        viewportFrameDeltaSeconds,
                                    .mouseDeltaX =
                                        viewportRightGestureDragged
                                            ? static_cast<
                                                  orbit::f64>(
                                                  deltaX)
                                            : 0.0,
                                    .mouseDeltaY =
                                        viewportRightGestureDragged
                                            ? static_cast<
                                                  orbit::f64>(
                                                  deltaY)
                                            : 0.0,
                                    .moveRight =
                                        moveRight,
                                    .moveForward =
                                        moveForward,
                                    .moveUp =
                                        moveUp,
                                    .boost =
                                        window.KeyDown(
                                            orbit::platform::
                                                Key::
                                                    LeftShift)
                                }));
                }

                if (viewportRightGestureActive &&
                    !rightMouseDown)
                {
                    if (window.RelativeMouseMode())
                    {
                        window.SetRelativeMouseMode(
                            false);
                    }

                    viewportRadialOpen =
                        !viewportRightGestureDragged;

                    viewportRightGestureActive =
                        false;
                    viewportRightGestureDragged =
                        false;
                    viewportRightGestureDistance =
                        0;
                }

                const bool homeDown =
                    window.KeyDown(
                        orbit::platform::
                            Key::Home);

                if (interaction.hovered &&
                    homeDown &&
                    !viewportHomeWasDown)
                {
                    static_cast<void>(
                        studioViews.
                            FocusTerrainBody(
                                "studio.primary"));
                }

                viewportHomeWasDown =
                    homeDown;

                const bool endDown =
                    window.KeyDown(
                        orbit::platform::
                            Key::End);

                if (interaction.hovered &&
                    endDown &&
                    !viewportEndWasDown)
                {
                    static_cast<void>(
                        studioViews.
                            ResetTerrainView(
                                "studio.primary"));
                }

                viewportEndWasDown =
                    endDown;

                const bool hasAuthoredBody =
                    bodyObject.IsValid() &&
                    bodyId.IsValid() &&
                    bodies().FindBody(bodyId) !=
                        nullptr;

                if (!hasAuthoredBody)
                {
                    pathPlacementMode = false;
                    lastPlacedPathNode.reset();
                    context.Text(
                        "No celestial body is authored in this world.");
                    context.Text(
                        "Create a celestial system/body from Explorer or automation.");
                    return;
                }

                const auto* primaryTarget =
                    studioSession.Viewports().Find(
                        "studio.primary");
                const bool primaryShowsPlanetMap =
                    primaryTarget != nullptr &&
                    (primaryTarget->mode ==
                         orbit::studio_session::
                             ViewportMode::FlatMap ||
                     primaryTarget->mode ==
                         orbit::studio_session::
                             ViewportMode::BodyMap);

                if (interaction.doubleClicked &&
                    !pathPlacementMode &&
                    primaryShowsPlanetMap)
                {
                    // Double-click on the flat map or the globe travels
                    // there and returns to the perspective view.
                    if (const auto direction =
                            studioViews.PickPlanetDirection(
                                "studio.primary",
                                interaction.u,
                                interaction.v);
                        direction.has_value() &&
                        studioViews.FocusTerrainDirection(
                            "studio.primary",
                            *direction))
                    {
                        studioSession.Viewports().SetMode(
                            "studio.primary",
                            orbit::studio_session::
                                ViewportMode::Perspective);
                    }
                }
                else if (interaction.doubleClicked &&
                    !pathPlacementMode)
                {
                    static_cast<void>(
                        studioViews.
                            FocusTerrainSurfacePoint(
                                "studio.primary",
                                interaction.u,
                                interaction.v));
                }

                if (const auto payload =
                        context.AcceptDragPayload(
                            "ORBIT_ASSET");
                    payload.has_value())
                {
                    if (const auto assetId =
                            DecodeAssetId(
                                *payload);
                        assetId.has_value())
                    {
                        const auto* asset =
                            content.Find(
                                *assetId);

                        if (asset != nullptr &&
                            (asset->kind ==
                                 orbit::content::
                                     AssetKind::Material ||
                             asset->kind ==
                                 orbit::content::
                                     AssetKind::MaterialInstance))
                        {
                            const std::array selected{
                                bodyObject
                            };

                            selection().Set(
                                std::span(
                                    selected));

                            try
                            {
                                authoringCommands().
                                    Invoke(
                                        orbit::editor_model::
                                            authoring_commands::
                                                kAssignMaterial,
                                        {
                                            {
                                                "material",
                                                asset->
                                                    sourcePath.
                                                    generic_string()
                                            }
                                        });
                            }
                            catch (const std::exception&
                                       exception)
                            {
                                orbit::log::Warning(
                                    exception.what());
                            }
                        }
                        else if (
                            asset != nullptr &&
                            asset->kind ==
                                orbit::content::AssetKind::Decal &&
                            asset->decal.has_value())
                        {
                            try
                            {
                                const auto* body =
                                    bodies().FindBody(bodyId);
                                const auto ray =
                                    orbit::render_view::ViewportRay(
                                        primaryView->Camera(),
                                        primaryView->Width(),
                                        primaryView->Height(),
                                        interaction.u,
                                        interaction.v);

                                if (body == nullptr || !ray.has_value())
                                {
                                    throw std::runtime_error(
                                        "Decal drop could not construct a body-local view ray.");
                                }

                                const auto hit =
                                    orbit::universe::IntersectReferenceSurfaceRay(
                                        body->shape,
                                        ray->origin,
                                        ray->direction);

                                if (!hit.has_value())
                                {
                                    throw std::runtime_error(
                                        "Decal drop did not hit the active body.");
                                }

                                const auto coordinate =
                                    orbit::universe::ReferenceSurfaceCoordinate(
                                        body->shape,
                                        *hit);

                                if (!coordinate.has_value())
                                {
                                    throw std::runtime_error(
                                        "Decal drop could not resolve a surface coordinate.");
                                }

                                const std::array selected{
                                    bodyObject
                                };
                                selection().Set(std::span(selected));

                                authoringCommands().Invoke(
                                    orbit::editor_model::authoring_commands::kAttachDecal,
                                    {
                                        {"decal", asset->sourcePath.generic_string()},
                                        {"latitude", coordinate->latitudeRadians},
                                        {"longitude", coordinate->longitudeRadians},
                                        {"width", asset->decal->widthMeters},
                                        {"height", asset->decal->heightMeters},
                                        {"rotation", 0.0},
                                        {"opacity", asset->decal->opacity}
                                    });
                            }
                            catch (const std::exception& exception)
                            {
                                orbit::log::Warning(exception.what());
                            }
                        }
                        else
                        {
                            orbit::log::Warning(
                                "Viewport drop expects a material or decal asset.");
                        }
                    }
                }

                if (interaction.clicked &&
                    pathPlacementMode)
                {
                    try
                    {
                        const auto* body =
                            bodies().FindBody(
                                bodyId);

                        const auto ray =
                            orbit::render_view::
                                ViewportRay(
                                    primaryView->Camera(),
                                    primaryView->Width(),
                                    primaryView->Height(),
                                    interaction.u,
                                    interaction.v);

                        if (body == nullptr ||
                            !ray.has_value())
                        {
                            throw std::runtime_error(
                                "Path placement could not construct a body-local view ray.");
                        }

                        const auto hit =
                            orbit::universe::
                                IntersectReferenceSurfaceRay(
                                    body->shape,
                                    ray->origin,
                                    ray->direction);

                        if (!hit.has_value())
                        {
                            throw std::runtime_error(
                                "Path placement ray did not hit the active body's reference surface.");
                        }

                        const auto coordinate =
                            orbit::universe::
                                ReferenceSurfaceCoordinate(
                                    body->shape,
                                    *hit);

                        if (!coordinate.has_value())
                        {
                            throw std::runtime_error(
                                "Path placement could not resolve the body surface coordinate.");
                        }

                        auto& pathService =
                            studioSession.PathNetwork().Service();

                        std::optional<
                            orbit::paths::NetworkId>
                            targetNetwork;

                        if (selection().Ordered().
                                size() == 1)
                        {
                            const auto selectedObject =
                                objects().Find(
                                    selection().Ordered().
                                        front());

                            if (selectedObject.
                                    has_value())
                            {
                                if (selectedObject->
                                        type ==
                                    orbit::paths::
                                        kPathNetworkType)
                                {
                                    targetNetwork =
                                        orbit::paths::
                                            NetworkId{
                                                .high =
                                                    selectedObject->
                                                        id.high,
                                                .low =
                                                    selectedObject->
                                                        id.low
                                            };
                                }
                                else if (
                                    selectedObject->
                                            type ==
                                        orbit::paths::
                                            kPathNodeType &&
                                    selectedObject->
                                        parent.
                                        has_value())
                                {
                                    targetNetwork =
                                        orbit::paths::
                                            NetworkId{
                                                .high =
                                                    selectedObject->
                                                        parent->
                                                        high,
                                                .low =
                                                    selectedObject->
                                                        parent->
                                                        low
                                            };
                                }
                            }
                        }

                        if (!targetNetwork.
                                has_value() &&
                            activePathNetwork.
                                has_value() &&
                            pathService.
                                FindNetwork(
                                    *activePathNetwork).
                                has_value())
                        {
                            targetNetwork =
                                activePathNetwork;
                        }

                        commandService().
                            BeginTransaction(
                                "Place Path Node");

                        try
                        {
                            if (!targetNetwork.
                                    has_value())
                            {
                                const auto network =
                                    pathService.
                                        CreateNetwork(
                                            "Path Network",
                                            bodyObject);

                                targetNetwork =
                                    network.id;
                            }

                            const orbit::scene::
                                ObjectId networkObject{
                                    .high =
                                        targetNetwork->
                                            high,
                                    .low =
                                        targetNetwork->
                                            low
                                };

                            orbit::u32 nodeCount = 0;

                            for (const auto& child :
                                 objects().Children(
                                     networkObject))
                            {
                                if (child.type ==
                                    orbit::paths::
                                        kPathNodeType)
                                {
                                    ++nodeCount;
                                }
                            }

                            const auto node =
                                pathService.
                                    CreateNode(
                                        *targetNetwork,
                                        std::format(
                                            "Path Node {}",
                                            nodeCount +
                                                1U),
                                        orbit::paths::
                                            SurfaceAnchor{
                                                .body =
                                                    bodyId,
                                                .coordinate = {
                                                    coordinate->
                                                        latitudeRadians,
                                                    coordinate->
                                                        longitudeRadians,
                                                    0.0
                                                }
                                            });

                            commandService().
                                CommitTransaction();

                            activePathNetwork =
                                targetNetwork;

                            if (lastPlacedPathNode.
                                    has_value())
                            {
                                const auto previous =
                                    pathService.
                                        FindNode(
                                            *lastPlacedPathNode);

                                if (previous.
                                        has_value() &&
                                    previous->network ==
                                        *targetNetwork)
                                {
                                    const std::array
                                        selected{
                                            *lastPlacedPathNode,
                                            node.id
                                        };

                                    selection().Set(
                                        std::span(
                                            selected));
                                }
                                else
                                {
                                    const std::array
                                        selected{
                                            node.id
                                        };

                                    selection().Set(
                                        std::span(
                                            selected));
                                }
                            }
                            else
                            {
                                const std::array
                                    selected{
                                        node.id
                                    };

                                selection().Set(
                                    std::span(
                                        selected));
                            }

                            lastPlacedPathNode =
                                node.id;
                        }
                        catch (...)
                        {
                            if (commandService().
                                    HasActiveTransaction())
                            {
                                commandService().
                                    RollbackTransaction();
                            }

                            throw;
                        }
                    }
                    catch (const std::exception&
                               exception)
                    {
                        orbit::log::Warning(
                            exception.what());
                    }
                }
                else if (
                    interaction.clicked ||
                    viewportRadialOpen)
                {
                    const std::array selected{
                        bodyObject
                    };

                    selection().Set(
                        std::span(
                            selected));
                }

                const auto radial =
                    presentActions(
                        "viewport",
                        orbit::editor_model::
                            CommandSurfaceKind::
                                Radial);

                context.RadialMenu(
                    "ViewportRadial",
                    radial,
                    viewportRadialOpen);

                const auto body =
                    objects().Find(
                        bodyObject);

                if (body.has_value())
                {
                    context.Text(
                        std::format(
                            "{}{}",
                            body->name,
                            selection().Contains(
                                bodyObject)
                                ? "  [selected]"
                                : ""));
                }
            }
    });
}
} // namespace orbit::editor_app
