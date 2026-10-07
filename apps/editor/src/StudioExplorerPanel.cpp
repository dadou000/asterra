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

void StudioExplorerPanel::Register()
{
    ui.RegisterPanel({
        .id = kExplorerPanel,
        .title = "Explorer",
        .defaultOpen = true,
        .defaultDock = orbit::editor_ui::DockRegion::Left,
        .dockOrder = 0,
        .draw =
            [this](
                orbit::editor_ui::
                    PanelContext& context)
            {
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

                const auto attempt =
                    [](const std::string_view what,
                       const auto& action)
                    {
                        try
                        {
                            action();
                            explorerStatus =
                                std::string(what) +
                                " done.";
                        }
                        catch (
                            const std::exception&
                                exception)
                        {
                            explorerStatus =
                                std::string(what) +
                                " failed: " +
                                exception.what();
                            orbit::log::Warning(
                                exception.what());
                        }
                    };

                static constexpr
                    std::string_view
                        kObjectPayload =
                            "ORBIT_OBJECT";

                static_cast<void>(
                    context.InputText(
                        "Search",
                        explorerSearch));

                context.Separator();

                // Explicit root drop target permits reparenting to
                // the world root without a special mutation path.
                static_cast<void>(
                    context.Selectable(
                        "World Root##root-drop",
                        false));

                if (const auto payload =
                        context.AcceptDragPayload(
                            kObjectPayload);
                    payload.has_value())
                {
                    if (const auto id =
                            DecodeObjectId(
                                *payload);
                        id.has_value())
                    {
                        try
                        {
                            explorer().Reparent(
                                *id,
                                std::nullopt);
                        }
                        catch (
                            const std::exception&
                                exception)
                        {
                            orbit::log::Warning(
                                exception.what());
                        }
                    }
                }

                if (!explorerSearch.empty())
                {
                    for (const auto& object :
                         explorer().Search(
                             explorerSearch))
                    {
                        const std::string label =
                            object.name +
                            "##search-" +
                            object.id.ToString();

                        if (context.Selectable(
                                label,
                                selection().Contains(
                                    object.id)))
                        {
                            explorer().Select(
                                object.id,
                                context.
                                    ControlDown());
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
                            const std::string label =
                                object.name +
                                "  -  " +
                                typeLabel(
                                    object.type) +
                                "##tree-" +
                                object.id.ToString();

                            const auto item =
                                context.TreeItem(
                                    label,
                                    selection().
                                        Contains(
                                            object.id));

                            if (item.clicked)
                            {
                                explorer().Select(
                                    object.id,
                                    context.
                                        ControlDown());
                            }

                            if (item.rightClicked &&
                                !selection().Contains(
                                    object.id))
                            {
                                explorer().Select(
                                    object.id,
                                    false);
                            }

                            const auto objectMenu =
                                presentActions(
                                    "explorer",
                                    orbit::editor_model::
                                        CommandSurfaceKind::
                                            ContextMenu);

                            if (context.BeginPopup(
                                    "ExplorerObjectMenu##" +
                                        object.id.
                                            ToString(),
                                    item.rightClicked))
                            {
                                context.MutedText(
                                    "Add child");

                                static constexpr std::array<
                                    orbit::schema::TypeId,
                                    6>
                                    kAddChildTypes{
                                        orbit::world_model::
                                            kCelestialSystemType,
                                        orbit::world_model::
                                            kCelestialBodyType,
                                        orbit::world_model::
                                            kCelestialReferenceNodeType,
                                        orbit::world_model::
                                            kPointLightType,
                                        orbit::world_model::
                                            kSpotLightType,
                                        orbit::world_model::
                                            kVisibilityProxyType
                                    };

                                for (const auto type :
                                     kAddChildTypes)
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
                                                static_cast<
                                                    void>(
                                                    commandService()
                                                        .CreateObject(
                                                            type,
                                                            name,
                                                            object.id));
                                            });
                                        context.
                                            CloseCurrentPopup();
                                    }
                                }

                                context.Separator();

                                if (context.Selectable(
                                        "Duplicate##dup-" +
                                            object.id.
                                                ToString(),
                                        false))
                                {
                                    attempt(
                                        "Duplicate",
                                        [&]
                                        {
                                            static_cast<
                                                void>(
                                                commandService()
                                                    .DuplicateObject(
                                                        object.id));
                                        });
                                    context.
                                        CloseCurrentPopup();
                                }

                                if (context.Selectable(
                                        "Delete##del-" +
                                            object.id.
                                                ToString(),
                                        false))
                                {
                                    attempt(
                                        "Delete",
                                        [&]
                                        {
                                            commandService()
                                                .DeleteObject(
                                                    object.id);
                                        });
                                    context.
                                        CloseCurrentPopup();
                                }

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
                                    context.
                                        CloseCurrentPopup();
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
                                            explorer()
                                                .Reparent(
                                                    object.id,
                                                    std::nullopt);
                                        });
                                    context.
                                        CloseCurrentPopup();
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
                                                object.id);
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

                    for (const auto& root :
                         explorer().Roots())
                    {
                        drawObject(root);
                    }
                }

                if (!explorerStatus.empty())
                {
                    context.MutedText(
                        explorerStatus);
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

                if (context.Section(
                        "Assets##explorer-assets",
                        false))
                {
                    using orbit::content::AssetKind;
                    static constexpr std::array<
                        std::pair<
                            AssetKind,
                            std::string_view>,
                        10>
                        kAssetGroups{{
                            {AssetKind::Material,
                             "Materials"},
                            {AssetKind::MaterialInstance,
                             "Material Instances"},
                            {AssetKind::ShaderMaterial,
                             "Shader Materials"},
                            {AssetKind::ShadingShader,
                             "Shading Shaders"},
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
                             "Path Profiles"}
                        }};

                    const auto assets = content.All();

                    for (const auto& [kind, title] :
                         kAssetGroups)
                    {
                        std::vector<
                            const orbit::content::
                                AssetRecord*>
                            group;

                        for (const auto& asset : assets)
                        {
                            if (asset.kind == kind)
                            {
                                group.push_back(&asset);
                            }
                        }

                        if (group.empty())
                        {
                            continue;
                        }

                        const auto node =
                            context.TreeItem(
                                std::string(title) +
                                    "  (" +
                                    std::to_string(
                                        group.size()) +
                                    ")##explorer-asset-group-" +
                                    std::string(title),
                                false);

                        if (node.open)
                        {
                            for (const auto* asset :
                                 group)
                            {
                                static_cast<void>(
                                    context.Selectable(
                                        asset->name +
                                            "##explorer-asset-" +
                                            asset->id.ToString(),
                                        false));
                            }
                            context.TreePop();
                        }
                    }
                }

                context.Separator();

                const auto& selected =
                    selection().Ordered();

                if (selected.size() == 1)
                {
                    if (renameSelectionRevision !=
                        selection().Revision())
                    {
                        const auto object =
                            objects().Find(
                                selected.front());

                        renameBuffer =
                            object.has_value()
                                ? object->name
                                : std::string{};

                        renameSelectionRevision =
                            selection().Revision();
                    }

                    static_cast<void>(
                        context.InputText(
                            "Name",
                            renameBuffer));

                    if (context.Button(
                            "Rename"))
                    {
                        try
                        {
                            explorer().Rename(
                                selected.front(),
                                renameBuffer);
                        }
                        catch (
                            const std::exception&
                                exception)
                        {
                            orbit::log::Warning(
                                exception.what());
                        }
                    }
                }

                const auto toolbar =
                    presentActions(
                        "viewport",
                        orbit::editor_model::
                            CommandSurfaceKind::
                                Toolbar);

                context.Toolbar(toolbar);
            }
    });
}
} // namespace orbit::editor_app
