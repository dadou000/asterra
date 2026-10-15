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
#include <cctype>
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

void StudioExplorerPanel::Register()
{
    ui.RegisterPanel({
        .id = kExplorerPanel,
        .title = "Explorer Source",
        .defaultOpen = true,
        .defaultDock = orbit::editor_ui::DockRegion::Left,
        .dockOrder = 0,
        .draw =
            [this](
                orbit::editor_ui::
                    PanelContext& context)
            {
                using orbit::editor_ui::ToolbarIcon;
                inspectorTarget.viewId = "studio.primary";
                if (!worldSession.HasWorld())
                {
                    context.Text("No world is open.");
                    return;
                }

                static std::string explorerStatus;
                static std::string moveFilter;
                static std::optional<
                    orbit::scene::ObjectId>
                    moveSource;
                static bool openMovePicker = false;
                static int explorerFilter = 0;
                // Stable visible-row order from the previous frame is used
                // for Shift-selection in either the tree or search results.
                static std::vector<orbit::scene::ObjectId> visibleRows;
                static std::optional<orbit::scene::ObjectId> selectionAnchor;
                static std::string previousSearch;
                static int previousFilter = -1;
                std::vector<orbit::scene::ObjectId> currentVisibleRows;
                using orbit::editor_ui::ToolbarChoice;

                const auto typeLabel =
                    [this](
                        const orbit::schema::TypeId
                            type)
                        -> std::string
                    {
                        if (const auto* schema =
                                schemas().FindType(
                                    type);
                            schema != nullptr)
                        {
                            return std::string(
                                schema->displayName);
                        }
                        return "Object";
                    };

                const auto typeIcon =
                    [](const orbit::schema::TypeId type)
                    {
                        using namespace orbit::world_model;
                        if (type == kWorldType) return ToolbarIcon::World;
                        if (type == kCelestialSystemType) return ToolbarIcon::Rings;
                        if (type == kCelestialBodyType) return ToolbarIcon::Planet;
                        if (type == kCelestialReferenceNodeType) return ToolbarIcon::Frame;
                        if (type == kTerrainSurfaceType) return ToolbarIcon::Surface;
                        if (type == kSurfaceDecalType) return ToolbarIcon::Decal;
                        if (type == kPointLightType) return ToolbarIcon::PointLight;
                        if (type == kSpotLightType) return ToolbarIcon::SpotLight;
                        if (type == kVisibilityProxyType) return ToolbarIcon::Visibility;
                        if (type == kPrimitiveType) return ToolbarIcon::Box;
                        return ToolbarIcon::Procedural;
                    };

                const auto selectVirtual =
                    [this, &selectionAnchor](const StudioInspectorTargetKind kind)
                    {
                        selection().Clear();
                        selectionAnchor.reset();
                        inspectorTarget.kind = kind;
                    };

                const auto drawCameraTree = [&]
                {
                    static bool initialized = false;
                    if (!initialized)
                    {
                        context.SetNextTreeItemOpen(true);
                        initialized = true;
                    }
                    const auto camera = context.TreeItemWithIcon(
                        "Viewport Camera##virtual-camera",
                        inspectorTarget.kind == StudioInspectorTargetKind::ViewportCamera,
                        ToolbarIcon::Camera, 24.0F, 2.0F);
                    if (camera.clicked) selectVirtual(StudioInspectorTargetKind::ViewportCamera);
                    if (!camera.open) return;
                    const auto eye = context.TreeItemWithIcon(
                        "Eye Adaptation##virtual-camera-eye",
                        inspectorTarget.kind == StudioInspectorTargetKind::EyeAdaptation,
                        ToolbarIcon::Visibility, 24.0F, 2.0F);
                    if (eye.clicked) selectVirtual(StudioInspectorTargetKind::EyeAdaptation);
                    context.TreePop();
                };

                const auto drawLightingTree = [&]
                {
                    static bool initialized = false;
                    if (!initialized)
                    {
                        context.SetNextTreeItemOpen(true);
                        initialized = true;
                    }
                    const auto lighting = context.TreeItemWithIcon(
                        "Lighting##virtual-renderer",
                        inspectorTarget.kind == StudioInspectorTargetKind::LightingRenderer,
                        ToolbarIcon::Renderer, 24.0F, 2.0F);
                    if (lighting.clicked) selectVirtual(StudioInspectorTargetKind::LightingRenderer);
                    if (!lighting.open) return;
                    struct RendererChild { std::string_view label; ToolbarIcon icon; StudioInspectorTargetKind target; };
                    static constexpr std::array<RendererChild, 8> children{{
                        {"Global Illumination", ToolbarIcon::GlobalIllumination, StudioInspectorTargetKind::GlobalIllumination},
                        {"Direct Lighting & Shadows", ToolbarIcon::PointLight, StudioInspectorTargetKind::DirectLighting},
                        {"Reflections", ToolbarIcon::Visibility, StudioInspectorTargetKind::Reflections},
                        {"Atmosphere", ToolbarIcon::Atmosphere, StudioInspectorTargetKind::AtmosphereLighting},
                        {"Clouds", ToolbarIcon::Clouds, StudioInspectorTargetKind::CloudLighting},
                        {"Ocean & Surface", ToolbarIcon::Ocean, StudioInspectorTargetKind::SurfaceLighting},
                        {"Anti-Aliasing", ToolbarIcon::AntiAliasing, StudioInspectorTargetKind::AntiAliasing},
                        {"Renderer Diagnostics", ToolbarIcon::Properties, StudioInspectorTargetKind::LightingDiagnostics}}};
                    for (const auto& child : children)
                    {
                        const auto item = context.TreeItemWithIcon(
                            std::string(child.label) + "##virtual-renderer-child-" + std::to_string(static_cast<int>(child.target)),
                            inspectorTarget.kind == child.target, child.icon, 24.0F, 2.0F);
                        if (item.clicked) selectVirtual(child.target);
                    }
                    context.TreePop();
                };

                const auto attempt = [](const std::string_view what, const auto& action)
                {
                    try
                    {
                        action();
                        explorerStatus = std::string(what) + " done.";
                    }
                    catch (const std::exception& exception)
                    {
                        explorerStatus = std::string(what) + " failed: " + exception.what();
                        orbit::log::Warning(exception.what());
                    }
                };
                // Reuse one selection policy for hierarchical and search rows.
                // Shift selects a visible contiguous range; Ctrl toggles;
                // Ctrl+Shift extends the existing selection.
                const auto selectRow=[&](orbit::scene::ObjectId id)
                {
                    if(context.ShiftDown() && selectionAnchor)
                    {
                        const auto a=std::find(
                            visibleRows.begin(),visibleRows.end(),*selectionAnchor);
                        const auto b=std::find(
                            visibleRows.begin(),visibleRows.end(),id);
                        if(a!=visibleRows.end() && b!=visibleRows.end())
                        {
                            auto start=static_cast<std::size_t>(
                                std::distance(visibleRows.begin(),a));
                            auto end=static_cast<std::size_t>(
                                std::distance(visibleRows.begin(),b));
                            if(start>end)std::swap(start,end);
                            std::vector<orbit::scene::ObjectId> range;
                            if(context.ControlDown())
                                range=selection().Ordered();
                            range.insert(range.end(),
                                visibleRows.begin()+start,
                                visibleRows.begin()+end+1U);
                            selection().Set(range);
                            inspectorTarget.kind=
                                StudioInspectorTargetKind::WorldSelection;
                            return;
                        }
                    }
                    explorer().Select(id,context.ControlDown());
                    selectionAnchor=id;
                    inspectorTarget.kind=
                        StudioInspectorTargetKind::WorldSelection;
                };

                // Select hidden descendants without manually expanding
                // nested tree rows. Explicit command only: ordinary clicks
                // never surprise-select an entire hierarchy.
                const auto selectSubtree=[&](
                    const orbit::scene::ObjectId id,
                    const bool includeRoot,
                    const bool additive)
                {
                    std::vector<orbit::scene::ObjectId> next=
                        additive?selection().Ordered():
                            std::vector<orbit::scene::ObjectId>{};
                    std::vector<orbit::scene::ObjectId> stack;
                    std::unordered_set<orbit::scene::ObjectId> visited;
                    if(includeRoot)stack.push_back(id);
                    else
                    {
                        const auto children=explorer().Children(id);
                        for(auto it=children.rbegin();it!=children.rend();++it)
                            stack.push_back(it->id);
                    }
                    while(!stack.empty())
                    {
                        const auto current=stack.back();
                        stack.pop_back();
                        if(!visited.insert(current).second)continue;
                        next.push_back(current);
                        const auto children=explorer().Children(current);
                        for(auto it=children.rbegin();it!=children.rend();++it)
                            stack.push_back(it->id);
                    }
                    selection().Set(next);
                    selectionAnchor=id;
                    inspectorTarget.kind=
                        StudioInspectorTargetKind::WorldSelection;
                };

                const auto deleteSelected=[&]()
                {
                    std::vector<orbit::scene::ObjectId> targets;
                    for(const auto id:selection().Ordered())
                    {
                        const auto record=objects().Find(id);
                        if(!record)continue;
                        if(record->type==orbit::world_model::kWorldType)
                        {
                            explorerStatus="World root is protected; deselect it to delete other objects.";
                            return;
                        }
                        targets.push_back(id);
                    }
                    if(targets.empty())return;
                    std::optional<orbit::scene::ObjectId> fallbackParent;
                    if(targets.size()==1U)
                    {
                        if(const auto record=objects().Find(targets.front()))
                            fallbackParent=record->parent;
                    }
                    attempt("Delete hierarchy",[&]
                    {
                        commandService().DeleteObjects(targets);
                        selection().Clear();
                        selectionAnchor.reset();
                        if(fallbackParent && objects().Find(*fallbackParent))
                        {
                            explorer().Select(*fallbackParent,false);
                            selectionAnchor=*fallbackParent;
                        }
                        inspectorTarget.kind=
                            StudioInspectorTargetKind::WorldSelection;
                    });
                };
                static constexpr std::string_view kObjectPayload = "ORBIT_OBJECT";
                static constexpr std::array<ToolbarChoice, 3> kExplorerFilters{{
                    {"All", ToolbarIcon::Layers}, {"World", ToolbarIcon::World}, {"Assets", ToolbarIcon::Asset}}};
                ORBIT_PROFILE_SCOPE("Explorer.body");
                static_cast<void>(context.ToolbarChoices("explorer-filter", kExplorerFilters, explorerFilter, false));
                context.MutedText("Search");
                static_cast<void>(context.InputText("##explorer-search", explorerSearch, 2.0F));
                if(previousSearch!=explorerSearch || previousFilter!=explorerFilter)
                {
                    visibleRows.clear();
                    selectionAnchor.reset();
                    previousSearch=explorerSearch;
                    previousFilter=explorerFilter;
                }
                context.MutedText("Ctrl+click: toggle | Shift+click: range | Ctrl+A: visible | Delete: subtree");
                context.KeyValue("Selected",std::to_string(selection().Ordered().size()));
                if(explorerFilter!=2)
                {
                    if(context.Button("Select visible##explorer-select-visible"))
                    {
                        selection().Set(visibleRows);
                        if(!visibleRows.empty())
                            selectionAnchor=visibleRows.front();
                        inspectorTarget.kind=
                            StudioInspectorTargetKind::WorldSelection;
                    }
                    context.SameLine();
                    if(context.Button("Clear selection##explorer-clear-selection"))
                    {
                        selection().Clear();
                        selectionAnchor.reset();
                    }
                    if(selection().Ordered().size()==1U)
                    {
                        const auto id=selection().Ordered().front();
                        if(const auto record=objects().Find(id))
                        {
                            if(record->parent && context.Button("Select parent##explorer-parent"))
                            {
                                selection().Set(std::array{*record->parent});
                                selectionAnchor=*record->parent;
                            }
                            context.SameLine();
                            if(context.Button("Select subtree##explorer-select-subtree"))
                                selectSubtree(id,true,false);
                            context.SameLine();
                            if(context.Button("Select children##explorer-select-children"))
                                selectSubtree(id,false,false);
                        }
                    }
                    if(!selection().Ordered().empty() &&
                       context.Button("Delete selected (undoable)##explorer-delete-selected"))
                        deleteSelected();
                }
                context.Separator();

                if (explorerFilter != 2)
                {
                if (!explorerSearch.empty())
                {
                    for (const auto& object :
                         explorer().Search(
                             explorerSearch))
                    {
                        currentVisibleRows.push_back(object.id);
                        const std::string label = object.name +
                            "##search-" + object.id.ToString();

                        if (context.SelectableWithIcon(
                                label,
                                selection().Contains(
                                    object.id),
                                typeIcon(object.type),
                                4.0F))
                        {
                            selectRow(object.id);
                        }
                    }
                }
                else
                {
                    std::function<void(
                        const orbit::scene::
                            ObjectRecord&)>
                        drawObject;

                    drawObject =
                        [&](const orbit::scene::
                                ObjectRecord&
                                    object)
                        {
                            ORBIT_PROFILE_SCOPE("Explorer.node");
                            currentVisibleRows.push_back(object.id);
                            const std::string label =
                                object.name +
                                "##tree-" +
                                object.id.ToString();

                            if (object.type == orbit::world_model::kWorldType)
                            {
                                static bool worldRootInitialized = false;
                                if (!worldRootInitialized)
                                {
                                    context.SetNextTreeItemOpen(true);
                                    worldRootInitialized = true;
                                }
                            }

                            const auto item =
                                context.TreeItemWithIcon(
                                    label,
                                    selection().
                                        Contains(
                                            object.id),
                                    typeIcon(object.type),
                                    24.0F,
                                    2.0F);

                            if (item.clicked)
                                selectRow(object.id);

                            if (item.rightClicked &&
                                !selection().Contains(object.id))
                            {
                                // Context-click on an unselected row should
                                // focus just that row, independent of held
                                // Ctrl/Shift. Right-click an already-selected
                                // row keeps its bulk selection intact.
                                selection().Set(std::array{object.id});
                                selectionAnchor=object.id;
                                inspectorTarget.kind=
                                    StudioInspectorTargetKind::WorldSelection;
                            }

                            // Building the action list is not free; only do
                            // it for the node whose context menu is open.
                            static std::optional<orbit::scene::ObjectId>
                                menuObject;
                            if (item.rightClicked)
                            {
                                menuObject = object.id;
                            }
                            const bool menuForThisObject =
                                menuObject.has_value() &&
                                *menuObject == object.id;
                            const auto objectMenu =
                                menuForThisObject
                                    ? presentActions(
                                          "explorer",
                                          orbit::editor_model::
                                              CommandSurfaceKind::
                                                  ContextMenu)
                                    : decltype(presentActions(
                                          "explorer",
                                          orbit::editor_model::
                                              CommandSurfaceKind::
                                                  ContextMenu)){};

                            if (context.BeginPopup(
                                    "ExplorerObjectMenu##" +
                                        object.id.
                                            ToString(),
                                    item.rightClicked))
                            {
                                context.MutedText("Add element");

                                std::vector<orbit::schema::TypeId> addTypes;
                                using namespace orbit::world_model;
                                if (object.type == kWorldType)
                                {
                                    addTypes = {
                                        kCelestialSystemType,
                                        kPrimitiveType,
                                        kPointLightType,
                                        kSpotLightType,
                                        kVisibilityProxyType};
                                }
                                else if (object.type == kCelestialSystemType ||
                                         object.type == kCelestialReferenceNodeType)
                                {
                                    addTypes = {
                                        kCelestialBodyType,
                                        kCelestialReferenceNodeType,
                                        kPrimitiveType,
                                        kPointLightType,
                                        kSpotLightType,
                                        kVisibilityProxyType};
                                }
                                else if (object.type == kCelestialBodyType)
                                {
                                    addTypes = {
                                        kCelestialBodyType,
                                        kCelestialReferenceNodeType,
                                        kReferenceShapeCapabilityType,
                                        kMassPropertiesCapabilityType,
                                        kOrbitCapabilityType,
                                        kRotationCapabilityType,
                                        kGravityCapabilityType,
                                        kTerrainSurfaceType,
                                        kSurfaceCapabilityType,
                                        kAtmosphereCapabilityType,
                                        kOceanCapabilityType,
                                        kCloudLayerCapabilityType,
                                        kRingSystemCapabilityType,
                                        kRadiativeEmitterCapabilityType,
                                        kPhotosphereCapabilityType,
                                        kGiantAppearanceCapabilityType,
                                        kSmallBodyAppearanceCapabilityType,
                                        kMagnetosphereCapabilityType,
                                        kCometTailCapabilityType,
                                        kCompactObjectCapabilityType,
                                        kAccretionFlowCapabilityType,
                                        kPointLightType,
                                        kSpotLightType,
                                        kVisibilityProxyType};
                                    std::erase_if(
                                        addTypes,
                                        [&](const orbit::schema::TypeId type)
                                        {
                                            if (type == kCloudLayerCapabilityType)
                                            {
                                                return false;
                                            }
                                            return std::ranges::any_of(
                                                explorer().Children(object.id),
                                                [type](const auto& child)
                                                {
                                                    return child.type == type;
                                                });
                                        });
                                }
                                else if (object.type == kTerrainSurfaceType ||
                                         object.type == kPrimitiveType)
                                {
                                    addTypes = {kSurfaceDecalType};
                                }

                                for (const auto type : addTypes)
                                {
                                    const std::string name =
                                        typeLabel(type);

                                    if (context.Selectable(
                                            "+ " + name +
                                                "##add-" +
                                                object.id.
                                                    ToString() +
                                                "-" +
                                                type.ToString(),
                                            false))
                                    {
                                        attempt(
                                            "Add " + name,
                                            [&]
                                            {
                                                const auto created =
                                                    commandService().CreateObject(
                                                        type, name, object.id);
                                                explorer().Select(created, false);
                                                selectionAnchor=created;
                                                inspectorTarget.kind =
                                                    StudioInspectorTargetKind::WorldSelection;
                                            });
                                        context.
                                            CloseCurrentPopup();
                                    }
                                }

                                if (object.type != orbit::world_model::kWorldType &&
                                    explorer().Children(object.id).empty())
                                {
                                    context.Separator();
                                    if (context.Selectable(
                                            "Duplicate##dup-" + object.id.ToString(),
                                            false))
                                    {
                                        attempt(
                                            "Duplicate",
                                            [&]
                                            {
                                                static_cast<void>(
                                                    commandService().DuplicateObject(object.id));
                                            });
                                        context.CloseCurrentPopup();
                                    }
                                }

                                context.Separator();
                                if(context.Selectable(
                                    "Select subtree##select-subtree-"+object.id.ToString(),
                                    false))
                                {
                                    selectSubtree(object.id,true,context.ControlDown());
                                    context.CloseCurrentPopup();
                                }
                                if(context.Selectable(
                                    "Select children##select-children-"+object.id.ToString(),
                                    false))
                                {
                                    selectSubtree(object.id,false,context.ControlDown());
                                    context.CloseCurrentPopup();
                                }
                                if(object.type==orbit::world_model::kWorldType)
                                    context.MutedText("The world root is protected.");
                                else
                                {
                                    const bool batch=selection().Contains(object.id) &&
                                        selection().Ordered().size()>1U;
                                    const std::string label=batch ?
                                        "Delete selected hierarchies##del-" :
                                        "Delete hierarchy (including children)##del-";
                                    if(context.Selectable(label+object.id.ToString(),false))
                                    {
                                        if(!batch)
                                        {
                                            selection().Set(
                                                std::array{object.id});
                                            selectionAnchor=object.id;
                                        }
                                        deleteSelected();
                                        context.CloseCurrentPopup();
                                    }
                                }

                                if (object.type != orbit::world_model::kWorldType)
                                {
                                    context.Separator();
                                    if (context.Selectable(
                                        "Move under...##mv-" +
                                            object.id.
                                                ToString(),
                                        false))
                                    {
                                        moveSource = object.id;
                                        moveFilter.clear();
                                        openMovePicker = true;
                                        context.CloseCurrentPopup();
                                    }

                                    if (context.Selectable(
                                        "Move to root##mvroot-" +
                                            object.id.
                                                ToString(),
                                        false))
                                    {
                                        attempt(
                                            "Move to root",
                                            [&]
                                            {
                                                explorer().Reparent(object.id, std::nullopt);
                                            });
                                        context.CloseCurrentPopup();
                                    }
                                }

                                if (!objectMenu.empty())
                                {
                                    context.Separator();
                                    if (context.ActionList(
                                            objectMenu))
                                    {
                                        context.
                                            CloseCurrentPopup();
                                    }
                                }

                                context.EndPopup();
                            }
                            else if (menuForThisObject)
                            {
                                menuObject.reset();
                            }

                            if (const auto payload =
                                    context.
                                        AcceptDragPayload(
                                            kObjectPayload);
                                payload.has_value())
                            {
                                if (const auto id =
                                        DecodeObjectId(
                                            *payload);
                                    id.has_value() &&
                                    *id != object.id)
                                {
                                    try
                                    {
                                            explorer().
                                            Reparent(
                                                *id,
                                                object.type == orbit::world_model::kWorldType
                                                    ? std::nullopt
                                                    : std::optional{object.id});
                                    }
                                    catch (
                                        const std::
                                            exception&
                                                exception)
                                    {
                                        orbit::log::
                                            Warning(
                                                exception.
                                                    what());
                                    }
                                }
                            }

                            if (context.
                                    BeginDragSource())
                            {
                                const auto payload =
                                    EncodeObjectId(
                                        object.id);

                                context.
                                    SetDragPayload(
                                        kObjectPayload,
                                        std::span(
                                            payload));

                                context.Text(
                                    object.name);
                                context.
                                    EndDragSource();
                            }

                            if (item.open)
                            {
                                if (object.type == orbit::world_model::kWorldType)
                                {
                                    drawCameraTree();
                                    drawLightingTree();
                                }

                                for (const auto&
                                         child :
                                     explorer().Children(
                                         object.id))
                                {
                                    drawObject(
                                        child);
                                }

                                context.TreePop();
                            }
                        };

                    ORBIT_PROFILE_SCOPE("Explorer.tree");
                    for (const auto& root :
                         explorer().Roots())
                    {
                        drawObject(root);
                    }
                }

                visibleRows=std::move(currentVisibleRows);
                if(context.WindowFocusedForShortcuts())
                {
                    if(context.ControlDown() &&
                       context.KeyPressed(orbit::editor_ui::UiKey::A) &&
                       !visibleRows.empty())
                    {
                        selection().Set(visibleRows);
                        selectionAnchor=visibleRows.front();
                        inspectorTarget.kind=
                            StudioInspectorTargetKind::WorldSelection;
                    }
                    else if(context.KeyPressed(orbit::editor_ui::UiKey::Delete))
                        deleteSelected();
                }
                if (!explorerStatus.empty())
                    context.MutedText(explorerStatus);
                }

                if (context.BeginPopup(
                        "Move under##explorer-move-picker",
                        openMovePicker,
                        {320.0F, 280.0F}))
                {
                    openMovePicker = false;
                    context.Text(
                        "Move under...");
                    static_cast<void>(
                        context.InputText(
                            "Filter##explorer-move-filter",
                            moveFilter));
                    context.Separator();

                    for (const auto& candidate :
                         explorer().Search(
                             moveFilter,
                             32U))
                    {
                        if (moveSource.has_value() &&
                            candidate.id ==
                                *moveSource)
                        {
                            continue;
                        }

                        if (context.Selectable(
                                candidate.name +
                                    "  -  " +
                                    typeLabel(
                                        candidate.type) +
                                    "##move-target-" +
                                    candidate.id.
                                        ToString(),
                                false) &&
                            moveSource.has_value())
                        {
                            attempt(
                                "Move",
                                [&]
                                {
                                    explorer().Reparent(
                                        *moveSource,
                                        candidate.id);
                                });
                            context.CloseCurrentPopup();
                        }
                    }

                    context.EndPopup();
                }

                if (explorerFilter != 1)
                {
                    using orbit::content::AssetKind;
                    static constexpr std::array<
                        std::pair<
                            AssetKind,
                            std::string_view>,
                        12>
                        kAssetGroups{{
                            {AssetKind::Material,
                             "Materials"},
                            {AssetKind::MaterialInstance,
                             "Material Instances"},
                            {AssetKind::ShaderMaterial,
                             "Shader Materials"},
                            {AssetKind::ShadingShader,
                             "Shading Shaders"},
                            {AssetKind::ColorLut,
                             "Color LUTs"},
                            {AssetKind::Shader,
                             "Shaders"},
                            {AssetKind::Texture,
                             "Textures"},
                            {AssetKind::Decal,
                             "Decals"},
                            {AssetKind::Mesh,
                             "Meshes"},
                            {AssetKind::Component,
                             "Components"},
                            {AssetKind::PathProfile,
                             "Path Profiles"},
                            {AssetKind::Unknown,
                             "Other Assets"}
                        }};

                    const auto assets = content.All();
                    std::string query = explorerSearch;
                    std::ranges::transform(query, query.begin(), [](const unsigned char ch)
                    {
                        return static_cast<char>(std::tolower(ch));
                    });

                    const auto assetIcon = [](const AssetKind kind)
                    {
                        switch (kind)
                        {
                        case AssetKind::Texture: return ToolbarIcon::Asset;
                        case AssetKind::Material: return ToolbarIcon::Decal;
                        case AssetKind::MaterialInstance: return ToolbarIcon::Decal;
                        case AssetKind::Decal: return ToolbarIcon::Decal;
                        case AssetKind::Component: return ToolbarIcon::Procedural;
                        case AssetKind::Mesh: return ToolbarIcon::Box;
                        case AssetKind::PathProfile: return ToolbarIcon::Procedural;
                        case AssetKind::Shader: return ToolbarIcon::Decal;
                        case AssetKind::ColorLut: return ToolbarIcon::Decal;
                        case AssetKind::ShadingShader: return ToolbarIcon::Decal;
                        case AssetKind::ShaderMaterial: return ToolbarIcon::Decal;
                        case AssetKind::Unknown: return ToolbarIcon::More;
                        }
                        return ToolbarIcon::More;
                    };

                    const auto matchingAssetCount = std::ranges::count_if(
                        assets,
                        [&](const auto& asset)
                        {
                            const bool recognized = std::ranges::any_of(
                                kAssetGroups,
                                [&](const auto& entry) { return entry.first == asset.kind; });
                            if (!recognized)
                                return false;
                            if (query.empty())
                                return true;
                            std::string name = asset.name;
                            std::ranges::transform(name, name.begin(), [](const unsigned char ch)
                            {
                                return static_cast<char>(std::tolower(ch));
                            });
                            return name.find(query) != std::string::npos;
                        });
                    context.SetNextTreeItemOpen(true);
                    const auto assetRoot = context.TreeItemWithIcon(
                    "Assets  (" + std::to_string(matchingAssetCount) +
                            ")##explorer-assets-root",
                    false,
                    ToolbarIcon::Asset,
                    24.0F,
                    2.0F);
                    if (assetRoot.open)
                    {

                    for (const auto& [kind, title] :
                         kAssetGroups)
                    {
                        std::vector<
                            const orbit::content::
                                AssetRecord*>
                            group;

                        for (const auto& asset : assets)
                        {
                            if (asset.kind != kind)
                                continue;
                            std::string name = asset.name;
                            std::ranges::transform(name, name.begin(), [](const unsigned char ch)
                            {
                                return static_cast<char>(std::tolower(ch));
                            });
                            if (query.empty() || name.find(query) != std::string::npos)
                                group.push_back(&asset);
                        }

                        if (group.empty())
                        {
                            continue;
                        }

                        const auto node =
                            context.TreeItemWithIcon(
                                std::string(title) +
                                    "  (" +
                                    std::to_string(
                                        group.size()) +
                                    ")##explorer-asset-group-" +
                                    std::string(title),
                                false,
                                assetIcon(kind),
                                24.0F,
                                2.0F);

                        if (node.open)
                        {
                            for (const auto* asset :
                                 group)
                            {
                                const std::string label = asset->name +
                                    "##explorer-asset-" + asset->id.ToString();
                                static_cast<void>(context.SelectableWithIcon(
                                    label, false, assetIcon(asset->kind), 4.0F));
                            }
                            context.TreePop();
                        }
                    }

                    if (matchingAssetCount == 0)
                    {
                        context.Text(
                            query.empty()
                                ? "No assets in this project yet."
                                : "No assets match this search.");
                    }
                    context.TreePop();
                    }
                }

            }
    });
}
} // namespace orbit::editor_app
