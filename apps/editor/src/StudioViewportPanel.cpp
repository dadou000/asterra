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
#include <orbit/path_geometry/RoadCatalog.hpp>
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
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/studio_session/StudioTerrainRoundTripVerifier.hpp>
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
#include <fstream>
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
    // This debug presentation toggle is an editor preference, not world state.
    try
    {
        std::ifstream in(orbit::platform::UserDataDirectory() /
            "studio_road_outlines.txt");
        int visible=0;
        if(in >> visible && (visible==0 || visible==1))
            pathDebugVisualization=visible!=0;
    }
    catch(const std::exception&) {}
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
                        infrastructureDrawMode && pathPlacementMode
                            ? "Finish Road Tool [Esc]"
                            : pathPlacementMode ? "Path Tool: On" : "Path Tool"))
                {
                    pathPlacementMode =
                        !pathPlacementMode;

                    if (!pathPlacementMode)
                    {
                        ResetInfrastructureChain();
                        infrastructureDrawMode=false;
                    }
                }

                context.SameLine();
                context.Toolbar(toolbar);

                if(infrastructureDrawMode && pathPlacementMode)
                {
                    context.SameLine();
                    if(context.Button("New chain##viewport-road-new-chain"))
                    {
                        ResetInfrastructureChain();
                        infrastructureLastAction=
                            "New road chain ready. Click to place the first node.";
                        infrastructurePlacementError.clear();
                    }
                    context.SameLine();
                    context.Text(infrastructureCatalogId.empty()
                        ? "Reference path" : infrastructureCatalogId);
                    context.SameLine();
                    if(context.Checkbox("Snap nodes##viewport-road-snap",
                        infrastructureSnapNodes))
                    {
                        infrastructureLastAction=infrastructureSnapNodes ?
                            "Node snapping enabled (hold Ctrl to bypass)." :
                            "Node snapping disabled.";
                        if(infrastructureSnapPreferenceChanged)
                            infrastructureSnapPreferenceChanged(
                                infrastructureSnapNodes);
                    }
                }

                context.SameLine();
                if(context.Checkbox(
                    "Road neon outlines (debug)",
                    pathDebugVisualization))
                {
                    try
                    {
                        const auto path=orbit::platform::UserDataDirectory() /
                            "studio_road_outlines.txt";
                        std::filesystem::create_directories(path.parent_path());
                        std::ofstream out(path,std::ios::trunc);
                        if(out)out << (pathDebugVisualization ? 1 : 0) << '\n';
                    }
                    catch(const std::exception&) {}
                }

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
                                "Atmosphere Debug##viewport-atmo-debug"))
                        {
                            startCapture(
                                orbit::studio_ui::
                                    ViewportCaptureService::Kind::
                                        AtmosphereDebug);
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

                    context.SameLine();
                    if (context.Button("Full Screen (F11)##viewport-fullscreen"))
                    {
                        fullscreenToggleRequested = true;
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

                // Independent cloud backend selector. Keep it on its own row:
                // the viewport toolbar is already wider than narrow docks.
                // B1 is visibly experimental and is not the existing shader
                // compare (clouds.split_set compares two A variants).
                auto cloudBackendLayers = studioViews.TerrainLayers("studio.primary");
                if (context.Button(cloudBackendLayers.cloudBPreview
                        ? "Cloud renderer: B3M PREVIEW##cloud-backend"
                        : "Cloud renderer: A (reference)##cloud-backend"))
                {
                    cloudBackendLayers.cloudBPreview = !cloudBackendLayers.cloudBPreview;
                    studioViews.SetTerrainLayers("studio.primary", cloudBackendLayers);
                    if (cloudBackendLayers.cloudBPreview &&
                        studioViewportRenderer.CloudSplitEnabled())
                    {
                        static_cast<void>(studioViewportRenderer.SetCloudSplit(false, "", 0.5F));
                    }
                }
                // Report all silent reasons for an empty cloud frame.
                if (!cloudBackendLayers.clouds)
                    context.Text("Clouds disabled in view layers.");
                else if (cloudBackendLayers.bypassAtmosphere)
                    context.Text("Clouds blocked by atmosphere bypass.");
                else if (!cloudBackendLayers.cloudBPreview &&
                         cloudBackendLayers.macroGlobe &&
                         !cloudBackendLayers.fullClipmap)
                    context.Text("Cloud A near-field disabled in macro-globe mode.");
                else if (const auto status =
                         studioViewportRenderer.CloudDiagnostics("studio.primary");
                         !status.has_value())
                    context.Text("No ready cloud field: check enabled Cloud Layer "
                                 "capabilities, source, and field generation.");
                else
                {
                    const auto& d = *status;
                    context.Text("Cloud field: " +
                                 std::to_string(d.layerCount) + " layers; coverage " +
                                 std::to_string(d.meanCoverage) + "; optical depth " +
                                 std::to_string(d.meanOpticalDepth) +
                                 (d.gpuResident ? " [GPU ready]" : " [GPU pending]"));
                    if (d.meanCoverage < 0.005)
                        context.Text("Cloud field is almost empty: inspect "
                                     "coverage and weather inputs.");
                }
                if (cloudBackendLayers.cloudBPreview)
                {
                    // Two fully independent A/B3 diagnostics. Changing
                    // either only updates this viewport's layered settings;
                    // it cannot modify System A shaders or caches.
                    if (context.Button(cloudBackendLayers.cloudLightVolume
                            ? "B Sun cache: ON##b-sun-cache"
                            : "B Sun cache: OFF##b-sun-cache"))
                    {
                        cloudBackendLayers.cloudLightVolume =
                            !cloudBackendLayers.cloudLightVolume;
                        studioViews.SetTerrainLayers(
                            "studio.primary", cloudBackendLayers);
                    }
                    if (context.Button(cloudBackendLayers.bypassCloudShadow
                            ? "B receiver shadows: OFF##b-receiver-shadow"
                            : "B receiver shadows: ON##b-receiver-shadow"))
                    {
                        cloudBackendLayers.bypassCloudShadow =
                            !cloudBackendLayers.bypassCloudShadow;
                        studioViews.SetTerrainLayers(
                            "studio.primary", cloudBackendLayers);
                    }
                    if (cloudBackendLayers.cloudLab.enabled)
                    {
                        context.Text(
                            "B3: Cloud Lab override is not supported yet. "
                            "Disable lab for B3 previews.");
                    }
                    if (!studioViewportRenderer.UnifiedCloudInitError().empty())
                    {
                        context.Text(
                            "Cloud B shader error: " +
                            studioViewportRenderer.UnifiedCloudInitError());
                    }
                    else
                    {
                        context.Text(
                            "B3M preview: multiscale 3D cloud density and cached sunlight; "
                            "temporal reconstruction and full skylight pending.");
                    }
                }

                // End the toolbar row before measuring the viewport. Without
                // this, ImGui reports only the unused width after the Zoom
                // input, shrinking both the rendered image and its hit area.
                context.Separator();
                const auto available =
                    context.ContentAvailable();

                // Render scale (view layer setting): the view renders at
                // this fraction of the panel and is shown stretched to it.
                const orbit::f32 renderScale = std::clamp(
                    studioViews.TerrainLayers("studio.primary").renderScale,
                    0.25F,
                    1.0F);
                const bool scaledRender = renderScale < 0.999F;

                const orbit::u32 width =
                    scaledRender
                        ? std::max(
                              1U,
                              static_cast<orbit::u32>(std::lround(
                                  static_cast<double>(
                                      std::max(available.width, 1.0F)) *
                                  renderScale)))
                        : static_cast<orbit::u32>(
                              std::max(
                                  available.width,
                                  1.0F));

                const orbit::u32 height =
                    scaledRender
                        ? std::max(
                              1U,
                              static_cast<orbit::u32>(std::lround(
                                  static_cast<double>(
                                      std::max(available.height, 1.0F)) *
                                  renderScale)))
                        : static_cast<orbit::u32>(
                              std::max(
                                  available.height,
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

                const orbit::editor_ui::UiSize imageSize{
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
                };
                auto interaction = scaledRender
                    ? context.ImageFit(
                          primaryView->DisplayColor(),
                          available,
                          imageSize)
                    : context.Image(
                          primaryView->DisplayColor(),
                          imageSize);

                // Cloud split view: a draggable divider with an A / B label either side. While it is grabbed the press is not a
                // selection click.
                if (!studioViews.TerrainLayers("studio.primary").cloudBPreview &&
                    studioViewportRenderer.CloudSplitEnabled())
                {
                    const orbit::f32 shownWidth = scaledRender ? available.width : imageSize.width;
                    const orbit::f32 shownHeight = scaledRender ? available.height : imageSize.height;
                    const orbit::f32 dividerX = studioViewportRenderer.CloudSplitPosition() * shownWidth;
                    const auto pointer = context.LastItemPointer();
                    if (pointer.pressed && std::fabs(pointer.position.x - dividerX) < 10.0F)
                    {
                        cloudSplitGrabbed = true;
                    }
                    if (!pointer.down)
                    {
                        cloudSplitGrabbed = false;
                    }
                    if (cloudSplitGrabbed && shownWidth > 1.0F)
                    {
                        (void)studioViewportRenderer.SetCloudSplit(
                            true,
                            studioViewportRenderer.CloudSplitDefines(),
                            std::clamp(pointer.position.x / shownWidth, 0.02F, 0.98F));
                        interaction.clicked = false;
                        interaction.doubleClicked = false;
                    }
                    const auto shorten = [](const std::string& text)
                    {
                        return text.empty() ? std::string("shipped") : text.substr(0, 56);
                    };
                    const orbit::math::Float4 handle{1.0F, 1.0F, 1.0F, cloudSplitGrabbed ? 1.0F : 0.85F};
                    const std::array<orbit::editor_ui::OverlayDisc, 1> grip{
                        orbit::editor_ui::OverlayDisc{{dividerX, shownHeight * 0.5F}, 7.0F, handle, true}};
                    context.OverlayShapesOnLastItem({}, grip);
                    context.OverlayLabelOnLastItem(
                        {8.0F, std::max(shownHeight - 30.0F, 0.0F)}, {1.0F, 1.0F, 1.0F, 1.0F},
                        "A  " + shorten(studioViewportRenderer.CloudShaderDefines()));
                    context.OverlayLabelOnLastItem(
                        {std::min(dividerX + 10.0F, std::max(shownWidth - 120.0F, 0.0F)), std::max(shownHeight - 30.0F, 0.0F)},
                        {1.0F, 1.0F, 1.0F, 1.0F},
                        "B  " + shorten(studioViewportRenderer.CloudSplitDefines()));
                }

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

                if(infrastructureDrawMode && pathPlacementMode &&
                   interaction.hovered &&
                   context.KeyPressed(orbit::editor_ui::UiKey::Enter))
                {
                    // Enter provides a keyboard-first way to complete one
                    // chain while keeping the chosen tool and road preset.
                    ResetInfrastructureChain();
                    infrastructureLastAction=
                        "Chain finished. Click to begin another road.";
                    infrastructurePlacementError.clear();
                }
                if(infrastructureDrawMode && viewportRadialOpen)
                {
                    // Right click finishes the current continuous chain,
                    // but leaves the chosen construction tool armed.
                    ResetInfrastructureChain();
                    viewportRadialOpen=false;
                    infrastructureLastAction=
                        "Chain finished. Left-click to start a new road.";
                    infrastructurePlacementError.clear();
                }
                if(infrastructureDrawMode && interaction.hovered &&
                   context.KeyPressed(orbit::editor_ui::UiKey::Escape))
                {
                    // Escape exits construction entirely. The authored
                    // network is never discarded or rewritten.
                    pathPlacementMode=false;
                    infrastructureDrawMode=false;
                    ResetInfrastructureChain();
                    infrastructureLastAction="Road construction exited.";
                    infrastructurePlacementError.clear();
                }
                const bool hasAuthoredBody =
                    bodyObject.IsValid() &&
                    bodyId.IsValid() &&
                    bodies().FindBody(bodyId) !=
                        nullptr;

                if (!hasAuthoredBody)
                {
                    pathPlacementMode=false;
                    infrastructureDrawMode=false;
                    ResetInfrastructureChain();
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

                // Elevation shortcuts are edge-triggered, never per-frame
                // auto-repeat. Shift moves in five-metre increments.
                const bool pageUp=window.KeyDown(
                    orbit::platform::Key::PageUp);
                const bool pageDown=window.KeyDown(
                    orbit::platform::Key::PageDown);
                if(infrastructureDrawMode && pathPlacementMode &&
                   interaction.hovered)
                {
                    const double delta=window.KeyDown(
                        orbit::platform::Key::LeftShift)?5.0:1.0;
                    if(pageUp && !infrastructurePageUpWasDown)
                        infrastructureElevationMeters=std::min(100000.0,
                            infrastructureElevationMeters+delta);
                    if(pageDown && !infrastructurePageDownWasDown)
                        infrastructureElevationMeters=std::max(-10000.0,
                            infrastructureElevationMeters-delta);
                }
                infrastructurePageUpWasDown=pageUp;
                infrastructurePageDownWasDown=pageDown;
                // Road placement follows the authored composed terrain instead
                // of the ellipsoid. Preview and committed nodes share this
                // sample, avoiding two competing altitude conventions.
                const auto roadSurfacePoint =
                    [&](const orbit::universe::BodyId targetBody,
                        const orbit::math::Double3 referencePoint)
                        -> orbit::math::Double3
                {
                    const auto radial=orbit::math::Normalize(referencePoint);
                    double elevation=infrastructureElevationMeters;
                    const auto& registry=studioSession.World().Surfaces().Registry();
                    const auto planet=registry.SphericalPlanetDefinition(targetBody);
                    const auto* surface=registry.FindTerrainSurface(targetBody);
                    if(planet && surface && surface->terrain)
                    {
                        const auto sample=surface->terrain->Sample({
                            .unitDirection=radial,
                            .footprintMeters=1.0,
                            .planet=planet->id,
                            .radialOffsetMeters=0.0});
                        if(std::isfinite(sample.elevationMeters))
                            elevation+=sample.elevationMeters;
                    }
                    return referencePoint+radial*elevation;
                };
                // One canonical handle computation for both preview and
                // authored Bezier. The second click selects the outgoing
                // direction in the local ground tangent plane; its distance
                // sets handle strength, bounded to half the segment chord.
                // The end tangent defaults to the end-to-start chord and
                // can still be edited in the expert editor.
                const auto curveHandles =
                    [](const orbit::math::Double3 start,
                       const orbit::math::Double3 directionPoint,
                       const orbit::math::Double3 end)
                    {
                        const auto up=orbit::math::Normalize(start);
                        const auto requested=directionPoint-start;
                        const auto tangent=requested-
                            up*orbit::math::Dot(requested,up);
                        const double selectedLength=orbit::math::Length(tangent);
                        const double chord=orbit::math::Length(end-start);
                        const double length=std::min(selectedLength,chord*0.5);
                        const auto out=selectedLength>1.0e-6 ?
                            tangent*(length/selectedLength) :
                            (end-start)*0.33;
                        return std::pair{out,(start-end)*0.33};
                    };

                // Direction picks belong to a specific start node. If a
                // different tool or undo changed it, discard the pending step.
                if((!lastPlacedPathNode ||
                    infrastructureCurveDirectionNode!=*lastPlacedPathNode ||
                    infrastructureAlignmentMode!=1) &&
                   infrastructureCurveDirectionPoint)
                {
                    infrastructureCurveDirectionPoint.reset();
                    infrastructureCurveDirectionNode={};
                }
                // Cities-style live construction preview: use the exact
                // same surface ray, semantic network and snap radius that
                // the committed click will use. No authored state is touched.
                std::optional<orbit::paths::PathNodeRecord> previewSnap;
                std::optional<orbit::math::Double3> previewPoint;
                infrastructureHoverValid=false;
                infrastructurePreviewSnapped=false;
                infrastructurePreviewLengthMeters=0.0;
                if(infrastructureDrawMode && pathPlacementMode &&
                   interaction.hovered)
                {
                    const auto* activeBody=bodies().FindBody(bodyId);
                    const auto ray=orbit::render_view::ViewportRay(
                        primaryView->Camera(),primaryView->Width(),
                        primaryView->Height(),
                        1.0F-interaction.u,interaction.v);
                    if(activeBody && ray)
                        previewPoint=orbit::universe::IntersectReferenceSurfaceRay(
                            activeBody->shape,ray->origin,ray->direction);
                    if(previewPoint)
                    {
                        *previewPoint=roadSurfacePoint(bodyId,*previewPoint);
                        infrastructureHoverValid=true;
                        auto& service=studioSession.PathNetwork().Service();
                        std::optional<orbit::paths::NetworkId> network;
                        if(activePathNetwork &&
                           service.FindNetwork(*activePathNetwork))
                            network=activePathNetwork;
                        // Existing Explorer selection is a fallback only.
                        if(!network && selection().Ordered().size()==1U)
                        {
                            const auto selected=objects().Find(
                                selection().Ordered().front());
                            if(selected)
                            {
                                if(selected->type==orbit::paths::kPathNetworkType)
                                    network=orbit::paths::NetworkId{
                                        selected->id.high,selected->id.low};
                                else if(selected->type==orbit::paths::kPathNodeType &&
                                        selected->parent)
                                    network=orbit::paths::NetworkId{
                                        selected->parent->high,selected->parent->low};
                            }
                        }
                        // Ctrl bypasses snapping only while held; it does not
                        // change the user's saved snapping preference.
                        const bool pickingCurveDirection=
                            infrastructureAlignmentMode==1 &&
                            lastPlacedPathNode &&
                            !infrastructureCurveDirectionPoint;
                        if(!pickingCurveDirection && network &&
                           infrastructureSnapNodes &&
                           !window.KeyDown(orbit::platform::Key::LeftControl))
                        {
                            if(infrastructureSnapCacheRevision!=objects().Revision() ||
                               infrastructureSnapCacheNetwork!=*network ||
                               infrastructureSnapCacheFrame!=activeBody->frame)
                            {
                                infrastructureSnapCache.clear();
                                const orbit::scene::ObjectId parent{
                                    network->high,network->low};
                                for(const auto& child:objects().Children(parent))
                                {
                                    if(child.type!=orbit::paths::kPathNodeType)
                                        continue;
                                    const auto node=service.FindNode(child.id);
                                    if(!node)continue;
                                    const auto* fixed=std::get_if<
                                        orbit::paths::FramePointAnchor>(
                                            &node->anchor);
                                    if(!fixed || fixed->frame!=activeBody->frame)
                                        continue;
                                    infrastructureSnapCache.emplace_back(
                                        node->id,fixed->localMeters);
                                }
                                infrastructureSnapCacheRevision=objects().Revision();
                                infrastructureSnapCacheNetwork=*network;
                                infrastructureSnapCacheFrame=activeBody->frame;
                            }
                            orbit::scene::ObjectId closest{};
                            double distance=infrastructureNodeSnapMeters;
                            for(const auto& candidate:infrastructureSnapCache)
                            {
                                const double d=orbit::math::Length(
                                    candidate.second-*previewPoint);
                                if(d<distance)
                                {
                                    distance=d;
                                    closest=candidate.first;
                                }
                            }
                            if(closest)
                            {
                                previewSnap=service.FindNode(closest);
                                if(previewSnap)
                                {
                                    const auto* fixed=std::get_if<
                                        orbit::paths::FramePointAnchor>(
                                            &previewSnap->anchor);
                                    if(fixed)
                                    {
                                        previewPoint=fixed->localMeters;
                                        infrastructurePreviewSnapped=true;
                                    }
                                }
                            }
                        }

                        const auto scale=scaledRender ?
                            std::min(available.width/
                                std::max(imageSize.width,1.0F),
                                available.height/
                                std::max(imageSize.height,1.0F)) : 1.0F;
                        const float previewWidth=imageSize.width*scale;
                        const float previewHeight=imageSize.height*scale;
                        const auto screen=[&](const orbit::math::Double3 point)
                            ->std::optional<orbit::math::Float2>
                        {
                            const auto uv=orbit::render_view::ProjectToViewport(
                                primaryView->Camera(),primaryView->Width(),
                                primaryView->Height(),point);
                            if(!uv || uv->u<0.0F || uv->u>1.0F ||
                               uv->v<0.0F || uv->v>1.0F)
                                return std::nullopt;
                            return orbit::math::Float2{
                                (1.0F-uv->u)*previewWidth,
                                uv->v*previewHeight};
                        };
                        std::vector<orbit::editor_ui::OverlaySegment> strokes;
                        std::vector<orbit::editor_ui::OverlayDisc> markers;
                        const auto pointColor=infrastructureAlignmentMode==2 ?
                            orbit::math::Float4{1.0F,0.77F,0.38F,1.0F} :
                            infrastructurePreviewSnapped ?
                            orbit::math::Float4{0.35F,0.92F,0.65F,1.0F} :
                            orbit::math::Float4{0.48F,0.77F,1.0F,1.0F};
                        if(const auto pointer=screen(*previewPoint))
                            markers.push_back({*pointer,
                                infrastructurePreviewSnapped?9.0F:5.0F,
                                pointColor,false});

                        if(lastPlacedPathNode)
                        {
                            const auto last=service.FindNode(*lastPlacedPathNode);
                            const auto* fixed=last ?
                                std::get_if<orbit::paths::FramePointAnchor>(
                                    &last->anchor):nullptr;
                            if(fixed && fixed->frame==activeBody->frame)
                            {
                                const auto start=fixed->localMeters;
                                const auto end=*previewPoint;
                                const auto delta=end-start;
                                const auto length=orbit::math::Length(delta);
                                infrastructurePreviewLengthMeters=length;
                                if(length>0.05 && length<200000.0)
                                {
                                    const auto* catalog=orbit::path_geometry::
                                        FindRoadCatalog(infrastructureCatalogId);
                                    const double halfWidth=catalog?
                                        catalog->pavedWidthMeters*0.5:
                                        infrastructureCatalogId=="ballasted-rail"?
                                            2.0175:0.0;
                                    // Before the second click, the cursor
                                    // indicates the outgoing direction, not
                                    // a committed curve endpoint.
                                    const bool chooseDirection=
                                        infrastructureAlignmentMode==1 &&
                                        !infrastructureCurveDirectionPoint;
                                    if(chooseDirection)
                                    {
                                        const auto startScreen=screen(start);
                                        const auto targetScreen=screen(end);
                                        if(startScreen && targetScreen)
                                            strokes.push_back({
                                                *startScreen,*targetScreen,
                                                {1.0F,0.79F,0.38F,1.0F},2.4F});
                                    }
                                    const auto handles=
                                        infrastructureAlignmentMode==1 &&
                                        infrastructureCurveDirectionPoint ?
                                        curveHandles(start,
                                            *infrastructureCurveDirectionPoint,
                                            end) :
                                        std::pair{delta*0.33,delta*-0.33};
                                    const auto p1=start+handles.first;
                                    const auto p2=end+handles.second;
                                    auto centerPrevious=screen(start);
                                    std::optional<orbit::math::Float2> leftPrevious;
                                    std::optional<orbit::math::Float2> rightPrevious;
                                    if(!chooseDirection)
                                    for(int segment=0;segment<=24;++segment)
                                    {
                                        const double t=static_cast<double>(
                                            segment)/24.0;
                                        const double inv=1.0-t;
                                        orbit::math::Double3 point=start+delta*t;
                                        orbit::math::Double3 tangent=delta;
                                        if(infrastructureAlignmentMode==1)
                                        {
                                            point=start*(inv*inv*inv)+
                                                p1*(3.0*inv*inv*t)+
                                                p2*(3.0*inv*t*t)+end*(t*t*t);
                                            tangent=(p1-start)*(3.0*inv*inv)+
                                                (p2-p1)*(6.0*inv*t)+
                                                (end-p2)*(3.0*t*t);
                                        }
                                        const auto center=screen(point);
                                        if(segment>0 && centerPrevious && center &&
                                           (infrastructureAlignmentMode!=2 ||
                                            segment%3!=0))
                                            strokes.push_back({
                                                *centerPrevious,*center,
                                                pointColor,2.4F});
                                        centerPrevious=center;
                                        if(halfWidth>0.0 &&
                                           infrastructureAlignmentMode!=2)
                                        {
                                            // Reference-ellipsoid tangent
                                            // shoulders are a geometric guide,
                                            // not a terrain-conformance claim.
                                            const auto lateral=orbit::math::Cross(
                                                orbit::math::Normalize(point),
                                                tangent);
                                            const auto lateralLength=orbit::math::
                                                Length(lateral);
                                            if(lateralLength>1.0e-5)
                                            {
                                                const auto offset=lateral*
                                                    (halfWidth/lateralLength);
                                                const auto left=screen(point+offset);
                                                const auto right=screen(point-offset);
                                                const orbit::math::Float4 rim{
                                                    0.82F,0.92F,0.99F,0.9F};
                                                if(leftPrevious && left)
                                                    strokes.push_back({
                                                        *leftPrevious,*left,
                                                        rim,1.4F});
                                                if(rightPrevious && right)
                                                    strokes.push_back({
                                                        *rightPrevious,*right,
                                                        rim,1.4F});
                                                leftPrevious=left;
                                                rightPrevious=right;
                                            }
                                        }
                                    }
                                    if(const auto startScreen=screen(start))
                                        markers.push_back({
                                            *startScreen,5.0F,
                                            {1.0F,0.92F,0.62F,1.0F},true});
                                    // Once the direction is locked, show the
                                    // actual first Bezier control handle so
                                    // its tangent is clear before committing.
                                    if(infrastructureAlignmentMode==1 &&
                                       infrastructureCurveDirectionPoint)
                                    {
                                        const auto guideEnd=screen(p1);
                                        const auto guideStart=screen(start);
                                        if(guideStart && guideEnd)
                                        {
                                            strokes.push_back({
                                                *guideStart,*guideEnd,
                                                {1.0F,0.78F,0.31F,0.95F},1.8F});
                                            markers.push_back({
                                                *guideEnd,4.5F,
                                                {1.0F,0.78F,0.31F,1.0F},true});
                                        }
                                    }
                                }
                            }
                        }
                        if(!strokes.empty() || !markers.empty())
                            context.OverlayShapesOnLastItem(strokes,markers);
                        context.OverlayLabelOnLastItem(
                            {12.0F,12.0F},pointColor,
                            infrastructureAlignmentMode==1 ?
                                (!lastPlacedPathNode ?
                                    "Curve 1/3: Click to place start node" :
                                    !infrastructureCurveDirectionPoint ?
                                        "Curve 2/3: Click to set outgoing direction" :
                                        std::format("Curve 3/3: Click end node  |  {:.1f} m{}",
                                            infrastructurePreviewLengthMeters,
                                            infrastructurePreviewSnapped ? "  |  node snap" : "")) :
                                lastPlacedPathNode ?
                                    std::format("{}  |  {:.1f} m{}",
                                        infrastructureAlignmentMode==0?"Straight":"Route chord",
                                        infrastructurePreviewLengthMeters,
                                        infrastructurePreviewSnapped?"  |  node snap":"") :
                                    (infrastructurePreviewSnapped?
                                        "Start from existing node":"Click to set first point"));
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
                                    infrastructureDrawMode ?
                                        1.0F-interaction.u : interaction.u,
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

                        const auto authoredPoint=infrastructureDrawMode ?
                            roadSurfacePoint(bodyId,*hit) : *hit;
                        auto& pathService =
                            studioSession.PathNetwork().Service();

                        if(infrastructureDrawMode &&
                           infrastructureAlignmentMode==1 &&
                           lastPlacedPathNode &&
                           !infrastructureCurveDirectionPoint)
                        {
                            const auto previous=pathService.FindNode(
                                *lastPlacedPathNode);
                            const auto* fixed=previous ?
                                std::get_if<orbit::paths::FramePointAnchor>(
                                    &previous->anchor) : nullptr;
                            if(!fixed || fixed->frame!=body->frame)
                                throw std::runtime_error(
                                    "Curve start is no longer valid. Start a new chain.");
                            const auto radial=orbit::math::Normalize(
                                fixed->localMeters);
                            const auto selected=authoredPoint-fixed->localMeters;
                            const auto groundDirection=selected-
                                radial*orbit::math::Dot(selected,radial);
                            if(orbit::math::Length(groundDirection)<0.5)
                                throw std::invalid_argument(
                                    "Choose a direction at least 0.5 m from the start.");
                            infrastructureCurveDirectionPoint=authoredPoint;
                            infrastructureCurveDirectionNode=*lastPlacedPathNode;
                            infrastructureLastAction=
                                "Direction set (2/3). Click the end node to build the curve.";
                            infrastructurePlacementError.clear();
                            return; // No node, edge or undo entry on click two.
                        }

                        std::optional<
                            orbit::paths::NetworkId>
                            targetNetwork;

                        // The builder's chosen network takes priority.
                        if(infrastructureDrawMode && activePathNetwork &&
                           pathService.FindNetwork(*activePathNetwork))
                            targetNetwork=activePathNetwork;
                        if(!targetNetwork && selection().Ordered().
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

                        commandService().BeginTransaction(
                            infrastructureDrawMode ?
                                (lastPlacedPathNode ?
                                    "Build Road Segment" : "Start Road Chain") :
                                "Place Path Node");

                        try
                        {
                            if (!targetNetwork.
                                    has_value())
                            {
                                const auto network =
                                    pathService.
                                        CreateNetwork(
                                            infrastructureDrawMode ?
                                                infrastructureCatalogId=="ballasted-rail" ?
                                                    "Rail Network" :
                                                !infrastructureCatalogId.empty() ?
                                                    "Road Network" : "Path Network" :
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

                            orbit::paths::PathNodeRecord node{};
                            // Snap to the SAME semantic network, not to any
                            // nearby but topologically unrelated road.
                            if(infrastructureDrawMode &&
                               previewSnap &&
                               previewSnap->network==*targetNetwork)
                                node=*previewSnap;
                            if(!node.id)
                            {
                                node=pathService.CreateNode(
                                    *targetNetwork,
                                    std::format("Path Node {}",nodeCount+1U),
                                    infrastructureDrawMode ?
                                        orbit::paths::PathAnchor{
                                            orbit::paths::FramePointAnchor{
                                                .frame=body->frame,
                                                .localMeters=authoredPoint}} :
                                        orbit::paths::PathAnchor{
                                            orbit::paths::SurfaceAnchor{
                                                .body=bodyId,
                                                .coordinate={
                                                    coordinate->latitudeRadians,
                                                    coordinate->longitudeRadians,
                                                    0.0}}});
                            }

                            // Re-clicking the current snapped endpoint is a
                            // selection/no-op, not a degenerate zero-length
                            // road or an empty undo-history transaction.
                            if(infrastructureDrawMode && lastPlacedPathNode &&
                               *lastPlacedPathNode==node.id)
                            {
                                commandService().RollbackTransaction();
                                infrastructureLastAction=
                                    "Same endpoint: move the cursor to extend the road, or right-click to start another chain.";
                                return;
                            }
                            // Road meshes require a finite nondegenerate
                            // chord. Reject short segments before committing.
                            if(infrastructureDrawMode && lastPlacedPathNode)
                            {
                                const auto previous=pathService.FindNode(
                                    *lastPlacedPathNode);
                                const auto* a=previous?
                                    std::get_if<orbit::paths::FramePointAnchor>(
                                        &previous->anchor):nullptr;
                                const auto* b=std::get_if<
                                    orbit::paths::FramePointAnchor>(&node.anchor);
                                if(a && b && a->frame==b->frame &&
                                   orbit::math::Length(
                                       a->localMeters-b->localMeters)<0.5)
                                {
                                    throw std::invalid_argument(
                                        "Road segment is under 0.5 m. Move the endpoint further away.");
                                }
                            }

                            orbit::scene::ObjectId newBuiltEdge{};
                            // City-builder construction is opt-in: the
                            // ordinary Path Tool continues to place nodes.
                            // One undo transaction contains the new node,
                            // linked edge and infrastructure metadata.
                            if(infrastructureDrawMode &&
                               lastPlacedPathNode &&
                               *lastPlacedPathNode!=node.id)
                            {
                                const auto previous=pathService.FindNode(
                                    *lastPlacedPathNode);
                                if(previous && previous->network==*targetNetwork)
                                {
                                    orbit::paths::PathEdgeRecord connected{};
                                    if(infrastructureAlignmentMode==2)
                                        connected=pathService.ConnectRouted(
                                            previous->id,node.id,
                                            "Terrain-routed segment");
                                    else if(infrastructureAlignmentMode==1)
                                    {
                                        const auto* startFixed=std::get_if<
                                            orbit::paths::FramePointAnchor>(
                                                &previous->anchor);
                                        if(!startFixed ||
                                           !infrastructureCurveDirectionPoint ||
                                           infrastructureCurveDirectionNode!=previous->id)
                                            throw std::runtime_error(
                                                "Curve direction is missing. Click to set it before the endpoint.");
                                        const auto* endFixed=std::get_if<
                                            orbit::paths::FramePointAnchor>(
                                                &node.anchor);
                                        if(!endFixed ||
                                           endFixed->frame!=startFixed->frame)
                                            throw std::runtime_error(
                                                "Curve endpoint must be in the start node's body frame.");
                                        const auto handles=curveHandles(
                                            startFixed->localMeters,
                                            *infrastructureCurveDirectionPoint,
                                            endFixed->localMeters);
                                        connected=pathService.ConnectBezier(
                                            previous->id,node.id,
                                            handles.first,handles.second,
                                            "Curved segment");
                                    }
                                    else
                                        connected=pathService.ConnectDirect(
                                            previous->id,node.id,
                                            "Straight segment");
                                    if(!infrastructureCatalogId.empty())
                                        pathService.SetInfrastructureIntent(
                                            connected.id,
                                            infrastructureCatalogId,1,
                                            infrastructureConstructionMode,
                                            infrastructureDesignSpeedKmh,
                                            infrastructureEaseTurns);
                                    newBuiltEdge=connected.id;
                                }
                            }
                            commandService().CommitTransaction();
                            infrastructurePlacementError.clear();
                            if(infrastructureDrawMode)
                            {
                                if(newBuiltEdge)
                                {
                                    infrastructureLastEdge=newBuiltEdge;
                                    ++infrastructureBuiltEdges;
                                    infrastructureLastAction=std::format(
                                        "Built segment {} - {} committed this session.",
                                        newBuiltEdge.ToString(),
                                        infrastructureBuiltEdges);
                                }
                                else
                                    infrastructureLastAction=
                                        infrastructureAlignmentMode==1 ?
                                        "Start node placed (1/3). Click to set the outgoing direction." :
                                        "Starting point placed. Click the next point to construct a segment.";
                            }

                            if(infrastructureDrawMode &&
                               infrastructureAlignmentMode==1 && newBuiltEdge)
                            {
                                infrastructureCurveDirectionPoint.reset();
                                infrastructureCurveDirectionNode={};
                            }
                            activePathNetwork =
                                targetNetwork;

                            // Constructing roads must not change the editor
                            // selection: selecting newly authored nodes can
                            // trigger selection-driven camera framing/reset.
                            // Leave explicit node selection to normal path tools.
                            if(!infrastructureDrawMode)
                            {
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
                        if(infrastructureDrawMode)
                        {
                            infrastructurePlacementError=exception.what();
                            infrastructureLastAction.clear();
                        }
                        orbit::log::Warning(exception.what());
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
