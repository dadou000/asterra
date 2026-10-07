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

void StudioMaterialServicePanel::Register()
{
    ui.RegisterPanel({
        .id = kContentPanel,
        .title = "Material Service",
        .defaultOpen = true,
        .defaultDock = orbit::editor_ui::DockRegion::Right,
        .dockOrder = 20,
        .draw =
            [this](
                orbit::editor_ui::
                    PanelContext& context)
            {
                static constexpr
                    std::string_view
                        kAssetPayload =
                            "ORBIT_ASSET";

                static_cast<void>(
                    context.InputText(
                        "Search",
                        contentSearch));

                context.SameLine();

                if (context.Button(
                        "Rescan"))
                {
                    content.Scan();

                    for (const auto& diagnostic :
                         content.Diagnostics())
                    {
                        orbit::log::Warning(
                            std::format(
                                "Content '{}': {}",
                                diagnostic.
                                    sourcePath.
                                    generic_string(),
                                diagnostic.message));
                    }
                }

                context.SameLine();

                if (context.Button(
                        "Import PBR Set..."))
                {
                    try
                    {
                        const auto folder =
                            orbit::platform::
                                SelectFolder(
                                    window,
                                    {
                                        .title =
                                            "Import PBR Material Set"
                                    });

                        if (folder.has_value())
                        {
                            const auto assetId =
                                content.ImportPbrSet(
                                    *folder);

                            const auto* imported =
                                content.Find(
                                    assetId);

                            orbit::log::Info(
                                std::format(
                                    "Imported PBR material '{}'.",
                                    imported != nullptr
                                        ? imported->name
                                        : assetId.ToString()));
                        }
                    }
                    catch (const std::exception&
                               exception)
                    {
                        orbit::log::Warning(
                            std::format(
                                "PBR material import failed: {}",
                                exception.what()));
                    }
                }

                context.Separator();

                const auto previewAvailable =
                    context.ContentAvailable();
                const orbit::f32 previewWidth =
                    std::clamp(
                        previewAvailable.width,
                        180.0F,
                        520.0F);
                const orbit::f32 previewHeight =
                    previewWidth * 0.625F;

                const orbit::u32 previewPixelsWide =
                    static_cast<orbit::u32>(
                        std::max(previewWidth, 1.0F));
                const orbit::u32 previewPixelsHigh =
                    static_cast<orbit::u32>(
                        std::max(previewHeight, 1.0F));

                if (materialView.Width() != previewPixelsWide ||
                    materialView.Height() != previewPixelsHigh)
                {
                    pendingMaterialViewResize =
                        std::pair{
                            previewPixelsWide,
                            previewPixelsHigh};
                }

                context.Text(
                    materialPreviewAsset.has_value()
                        ? "Rendered material/decal preview"
                        : "Select a material, instance or decal to preview");
                static_cast<void>(
                    context.Image(
                        materialView.Color(),
                        {
                            .width = previewWidth,
                            .height = previewHeight
                        }));

                if (materialPreviewAsset.has_value())
                {
                    const auto* selectedAsset =
                        content.Find(
                            *materialPreviewAsset);

                    if (selectedAsset != nullptr &&
                        (selectedAsset->kind ==
                             orbit::content::AssetKind::Material ||
                         selectedAsset->kind ==
                             orbit::content::AssetKind::MaterialInstance))
                    {
                        if (!materialEmissionEditAsset.has_value() ||
                            *materialEmissionEditAsset !=
                                selectedAsset->id)
                        {
                            try
                            {
                                materialEmissionEdit =
                                    content.ResolveMaterialEmission(
                                        selectedAsset->id);
                                materialEmissionEditAsset =
                                    selectedAsset->id;

                                materialEmissiveTextureEdit.clear();

                                if (selectedAsset->kind ==
                                        orbit::content::AssetKind::
                                            Material &&
                                    selectedAsset->material.
                                        has_value())
                                {
                                    materialEmissiveTextureEdit =
                                        selectedAsset->material->
                                            emissive.
                                            generic_string();
                                }

                                materialEmissionStatus.clear();
                            }
                            catch (const std::exception& exception)
                            {
                                materialEmissionStatus =
                                    exception.what();
                            }
                        }

                        context.Separator();
                        context.Text(
                            std::format(
                                "Physical Emission — {}",
                                selectedAsset->name));

                        const orbit::content::AssetRecord*
                            baseMaterial =
                                selectedAsset;

                        for (orbit::u32 depth = 0U;
                             depth < 8U &&
                             baseMaterial != nullptr &&
                             baseMaterial->kind ==
                                 orbit::content::AssetKind::
                                     MaterialInstance;
                             ++depth)
                        {
                            if (!baseMaterial->
                                    materialInstance.
                                    has_value())
                            {
                                baseMaterial = nullptr;
                                break;
                            }

                            const auto parentPath =
                                baseMaterial->
                                    sourcePath.
                                    parent_path() /
                                baseMaterial->
                                    materialInstance->
                                    parent;

                            baseMaterial =
                                content.FindByPath(
                                    parentPath);
                        }

                        if (selectedAsset->kind ==
                            orbit::content::AssetKind::Material)
                        {
                            static_cast<void>(
                                context.InputText(
                                    "Emission Texture##m15-emission-texture",
                                    materialEmissiveTextureEdit));

                            context.MutedText(
                                "Project-relative path from this material folder. Leave empty for no emission texture.");
                        }
                        else
                        {
                            if (baseMaterial != nullptr &&
                                baseMaterial->material.
                                    has_value() &&
                                !baseMaterial->material->
                                    emissive.empty())
                            {
                                context.Text(
                                    "Inherited Emission Texture: " +
                                    (baseMaterial->sourcePath.
                                         parent_path() /
                                     baseMaterial->material->
                                         emissive).
                                        generic_string());
                            }
                            else
                            {
                                context.MutedText(
                                    "Inherited Emission Texture: none");
                            }

                            context.MutedText(
                                "Instance values start from inherited emission; Save writes explicit overrides. Texture remains inherited from the base material.");
                        }

                        bool changed = false;

                        changed |= context.InputDouble(
                            "Emission R##m15-emission-r",
                            materialEmissionEdit.
                                colorLinear[0]);
                        changed |= context.InputDouble(
                            "Emission G##m15-emission-g",
                            materialEmissionEdit.
                                colorLinear[1]);
                        changed |= context.InputDouble(
                            "Emission B##m15-emission-b",
                            materialEmissionEdit.
                                colorLinear[2]);
                        changed |= context.InputDouble(
                            "Luminance (nits)##m15-emission-nits",
                            materialEmissionEdit.
                                luminanceNits);
                        changed |= context.Checkbox(
                            "Contributes to GI##m15-emission-gi-enabled",
                            materialEmissionEdit.
                                contributesToGi);
                        changed |= context.InputDouble(
                            "GI Scale##m15-emission-gi-scale",
                            materialEmissionEdit.
                                giScale);

                        materialEmissionEdit.colorLinear[0] =
                            std::max(
                                materialEmissionEdit.
                                    colorLinear[0],
                                0.0);
                        materialEmissionEdit.colorLinear[1] =
                            std::max(
                                materialEmissionEdit.
                                    colorLinear[1],
                                0.0);
                        materialEmissionEdit.colorLinear[2] =
                            std::max(
                                materialEmissionEdit.
                                    colorLinear[2],
                                0.0);
                        materialEmissionEdit.luminanceNits =
                            std::max(
                                materialEmissionEdit.
                                    luminanceNits,
                                0.0);
                        materialEmissionEdit.giScale =
                            std::max(
                                materialEmissionEdit.
                                    giScale,
                                0.0);

                        const auto evaluated =
                            orbit::lighting::
                                EvaluateMaterialEmission({
                                    .colorLinear = {
                                        static_cast<orbit::f32>(
                                            materialEmissionEdit.
                                                colorLinear[0]),
                                        static_cast<orbit::f32>(
                                            materialEmissionEdit.
                                                colorLinear[1]),
                                        static_cast<orbit::f32>(
                                            materialEmissionEdit.
                                                colorLinear[2])
                                    },
                                    .luminanceNits =
                                        static_cast<orbit::f32>(
                                            materialEmissionEdit.
                                                luminanceNits),
                                    .contributesToGi =
                                        materialEmissionEdit.
                                            contributesToGi,
                                    .giScale =
                                        static_cast<orbit::f32>(
                                            materialEmissionEdit.
                                                giScale)
                                });

                        context.Text(
                            std::format(
                                "Visible radiance: [{:.4g}, {:.4g}, {:.4g}] W/(sr m^2)",
                                evaluated.visibleRadiance.x,
                                evaluated.visibleRadiance.y,
                                evaluated.visibleRadiance.z));

                        context.Text(
                            std::format(
                                "GI radiance: [{:.4g}, {:.4g}, {:.4g}] W/(sr m^2)",
                                evaluated.giRadiance.x,
                                evaluated.giRadiance.y,
                                evaluated.giRadiance.z));

                        context.MutedText(
                            "Bloom/glare are display effects and are not required for this material to emit scene radiance.");

                        if (changed)
                        {
                            materialPreviewMaterial.
                                emissionRadiance =
                                    evaluated.visibleRadiance;
                        }

                        if (context.PrimaryButton(
                                "Save Emission##m15-save-emission"))
                        {
                            try
                            {
                                // Content edits rescan the registry, so never
                                // retain AssetRecord pointers across a save.
                                const auto editedAssetId =
                                    selectedAsset->id;
                                const auto editedAssetKind =
                                    selectedAsset->kind;

                                if (editedAssetKind ==
                                    orbit::content::AssetKind::Material)
                                {
                                    content.SetMaterialEmissiveTexture(
                                        editedAssetId,
                                        std::filesystem::path(
                                            materialEmissiveTextureEdit));
                                }

                                content.SetMaterialEmission(
                                    editedAssetId,
                                    materialEmissionEdit);

                                const auto* refreshed =
                                    content.Find(
                                        editedAssetId);

                                materialPreviewMaterial =
                                    PreviewMaterialForAsset(
                                        content,
                                        refreshed);

                                materialEmissionStatus =
                                    "Physical emission saved.";
                            }
                            catch (const std::exception& exception)
                            {
                                materialEmissionStatus =
                                    exception.what();
                            }
                        }

                        const auto& selection =
                            studioSession.World().
                                Selection().
                                Ordered();

                        if (selection.size() == 1U)
                        {
                            if (context.Button(
                                    "Assign to Selected Object##m15-assign-runtime"))
                            {
                                try
                                {
                                    auto& world =
                                        studioSession.World();
                                    auto& commands =
                                        world.Commands();
                                    const auto owner =
                                        selection.front();

                                    std::optional<
                                        orbit::scene::ObjectId>
                                        existing;

                                    for (const auto& child :
                                         world.Objects().
                                             Children(owner))
                                    {
                                        if (child.type !=
                                            orbit::world_model::
                                                kMaterialAssignmentType)
                                        {
                                            continue;
                                        }

                                        std::string slot =
                                            "default";

                                        const auto slotValue =
                                            world.Objects().
                                                GetProperty(
                                                    child.id,
                                                    orbit::world_model::
                                                        kMaterialAssignmentSlot);

                                        if (slotValue.has_value())
                                        {
                                            if (const auto* text =
                                                    std::get_if<std::string>(
                                                        &*slotValue))
                                            {
                                                slot =
                                                    text->empty()
                                                        ? "default"
                                                        : *text;
                                            }
                                        }

                                        if (slot == "default")
                                        {
                                            existing =
                                                child.id;
                                            break;
                                        }
                                    }

                                    commands.BeginTransaction(
                                        "Assign Material");

                                    orbit::scene::ObjectId
                                        assignment{};

                                    if (existing.has_value())
                                    {
                                        assignment =
                                            *existing;
                                    }
                                    else
                                    {
                                        assignment =
                                            commands.CreateObject(
                                                orbit::world_model::
                                                    kMaterialAssignmentType,
                                                "Material Assignment",
                                                owner);

                                        commands.SetProperty(
                                            assignment,
                                            orbit::world_model::
                                                kMaterialAssignmentSlot,
                                            std::string{
                                                "default"});
                                    }

                                    commands.SetProperty(
                                        assignment,
                                        orbit::world_model::
                                            kMaterialAssignmentAsset,
                                        selectedAsset->id.
                                            ToString());

                                    commands.SetProperty(
                                        assignment,
                                        orbit::world_model::
                                            kMaterialAssignmentEnabled,
                                        true);

                                    commands.CommitTransaction();

                                    materialEmissionStatus =
                                        std::format(
                                            "Assigned '{}' to selected object.",
                                            selectedAsset->name);
                                }
                                catch (const std::exception&
                                           exception)
                                {
                                    if (studioSession.World().
                                            Commands().
                                            HasActiveTransaction())
                                    {
                                        studioSession.World().
                                            Commands().
                                            RollbackTransaction();
                                    }

                                    materialEmissionStatus =
                                        exception.what();
                                }
                            }

                            context.MutedText(
                                "Assignment is persistent/undoable and resolves Material Instance emission at runtime.");
                        }
                        else
                        {
                            context.MutedText(
                                "Select exactly one world object to assign this material.");
                        }

                        if (!materialEmissionStatus.empty())
                        {
                            context.Text(
                                materialEmissionStatus);
                        }
                    }
                }

                context.Separator();

                const auto assets =
                    content.Search(
                        contentSearch);

                context.Text(
                    std::format(
                        "{} asset{} indexed",
                        assets.size(),
                        assets.size() == 1
                            ? ""
                            : "s"));

                for (const auto& asset :
                     assets)
                {
                    const std::string label =
                        std::format(
                            "{}  [{}]##asset-{}",
                            asset.name,
                            orbit::content::
                                AssetKindName(
                                    asset.kind),
                            asset.id.ToString());

                    const bool previewSelected =
                        materialPreviewAsset.has_value() &&
                        *materialPreviewAsset == asset.id;

                    if (context.Selectable(
                            label,
                            previewSelected))
                    {
                        if (asset.kind ==
                                orbit::content::AssetKind::Material ||
                            asset.kind ==
                                orbit::content::AssetKind::MaterialInstance ||
                            asset.kind ==
                                orbit::content::AssetKind::Decal)
                        {
                            materialPreviewAsset = asset.id;
                            materialPreviewMaterial =
                                PreviewMaterialForAsset(
                                    content,
                                    &asset);
                            materialEmissionEditAsset.reset();
                            materialEmissiveTextureEdit.clear();
                            materialEmissionStatus.clear();
                        }
                    }

                    if (asset.kind ==
                        orbit::content::
                            AssetKind::Material)
                    {
                        context.SameLine();

                        const std::string
                            instanceLabel =
                                "Instance##asset-" +
                                asset.id.
                                    ToString();

                        if (context.Button(
                                instanceLabel))
                        {
                            try
                            {
                                const auto instanceId =
                                    content.
                                        CreateMaterialInstance(
                                            asset.id);

                                const auto* instance =
                                    content.Find(
                                        instanceId);

                                orbit::log::Info(
                                    std::format(
                                        "Created material instance '{}'.",
                                        instance != nullptr
                                            ? instance->name
                                            : instanceId.ToString()));
                            }
                            catch (const std::exception&
                                       exception)
                            {
                                orbit::log::Warning(
                                    std::format(
                                        "Material instance creation failed: {}",
                                        exception.what()));
                            }
                        }
                    }

                    if (asset.kind ==
                        orbit::content::AssetKind::Texture)
                    {
                        context.SameLine();

                        const std::string decalLabel =
                            "Create Decal##asset-" +
                            asset.id.ToString();

                        if (context.Button(decalLabel))
                        {
                            try
                            {
                                const auto decalId =
                                    content.CreateDecal(
                                        asset.id);
                                const auto* decal =
                                    content.Find(decalId);
                                materialPreviewAsset = decalId;
                                materialPreviewMaterial =
                                    PreviewMaterialForAsset(
                                        content,
                                        decal);

                                orbit::log::Info(
                                    std::format(
                                        "Created decal '{}'.",
                                        decal != nullptr
                                            ? decal->name
                                            : decalId.ToString()));
                            }
                            catch (const std::exception& exception)
                            {
                                orbit::log::Warning(
                                    std::format(
                                        "Decal creation failed: {}",
                                        exception.what()));
                            }
                        }
                    }

                    if (context.
                            BeginDragSource())
                    {
                        const auto payload =
                            EncodeAssetId(
                                asset.id);

                        context.SetDragPayload(
                            kAssetPayload,
                            std::span(
                                payload));

                        context.Text(
                            asset.name);
                        context.EndDragSource();
                    }

                    context.Text(
                        "  " +
                        asset.sourcePath.
                            generic_string());
                }

                if (!content.
                        Diagnostics().
                        empty())
                {
                    context.Separator();
                    context.Text(
                        std::format(
                            "{} indexing diagnostic{}",
                            content.Diagnostics().
                                size(),
                            content.Diagnostics().
                                    size() == 1
                                ? ""
                                : "s"));

                    for (const auto& diagnostic :
                         content.Diagnostics())
                    {
                        context.Text(
                            std::format(
                                "{}: {}",
                                diagnostic.
                                    sourcePath.
                                    generic_string(),
                                diagnostic.message));
                    }
                }
            }
    });
}
} // namespace orbit::editor_app
