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

StudioPlatformServicesPanel::StudioPlatformServicesPanel(StudioPanelEnvironment& environment)
    : StudioPanelBase(environment)
{
    if (std::filesystem::is_regular_file(
            platformConfigurationPath))
    {
        try
        {
            platformConfiguration =
                orbit::platform_services::
                    LoadPlatformConfiguration(
                        platformConfigurationPath);
            steamAppIdEditor =
                static_cast<orbit::i64>(
                    platformConfiguration.
                        steam.appId);
            platformConfigurationIssues =
                orbit::platform_services::
                    ValidatePlatformConfiguration(
                        platformConfiguration,
                        false);
            platformStatus =
                platformConfigurationIssues.
                        empty()
                    ? "Loaded"
                    : "Loaded with validation issues";
        }
        catch (const std::exception&
                   exception)
        {
            platformStatus =
                std::string(
                    "Load failed: ") +
                exception.what();
        }
    }
}

void StudioPlatformServicesPanel::Register()
{
    ui.RegisterPanel({
        .id = kPlatformServicesPanel,
        .title = "Platform Services",
        .defaultOpen = false,
        .defaultDock = orbit::editor_ui::DockRegion::Bottom,
        .dockOrder = 30,
        .draw =
            [this](
                orbit::editor_ui::
                    PanelContext& context)
            {
                context.Text(
                    "Provider-neutral IDs remain project authority; storefront mappings live here.");
                context.Separator();

                static_cast<void>(
                    context.Checkbox(
                        "Enable Steam",
                        platformConfiguration.
                            steam.enabled));
                static_cast<void>(
                    context.InputInteger(
                        "Steam App ID",
                        steamAppIdEditor));

                context.Text(
                    std::format(
                        "Achievements: {}  Stats: {}  Timeline events: {}",
                        platformConfiguration.
                            steam.achievements.
                            size(),
                        platformConfiguration.
                            steam.stats.size(),
                        platformConfiguration.
                            steam.timelineEvents.
                            size()));

                context.Separator();
                context.Text(
                    "Achievement mapping");
                static_cast<void>(
                    context.InputText(
                        "Orbit Achievement ID",
                        newAchievementId));
                static_cast<void>(
                    context.InputText(
                        "Steam Achievement API",
                        newAchievementApiName));
                if (context.Button(
                        "Add Achievement Mapping") &&
                    !newAchievementId.empty() &&
                    !newAchievementApiName.empty())
                {
                    platformConfiguration.
                        steam.achievements.
                        push_back({
                            .id =
                                newAchievementId,
                            .steamApiName =
                                newAchievementApiName
                        });
                    newAchievementId.clear();
                    newAchievementApiName.clear();
                }

                context.Separator();
                context.Text("Stat mapping");
                static_cast<void>(
                    context.InputText(
                        "Orbit Stat ID",
                        newStatId));
                static_cast<void>(
                    context.InputText(
                        "Steam Stat API",
                        newStatApiName));
                static_cast<void>(
                    context.Checkbox(
                        "Float Stat",
                        newStatFloat));
                if (context.Button(
                        "Add Stat Mapping") &&
                    !newStatId.empty() &&
                    !newStatApiName.empty())
                {
                    platformConfiguration.
                        steam.stats.push_back({
                            .id = newStatId,
                            .kind = newStatFloat
                                ? orbit::platform_services::
                                    StatKind::Float
                                : orbit::platform_services::
                                    StatKind::Integer,
                            .steamApiName =
                                newStatApiName
                        });
                    newStatId.clear();
                    newStatApiName.clear();
                }

                context.Separator();
                context.Text(
                    "Timeline event mapping");
                static_cast<void>(
                    context.InputText(
                        "Semantic Event ID",
                        newTimelineEventId));
                static_cast<void>(
                    context.InputText(
                        "Timeline Title",
                        newTimelineTitle));
                static_cast<void>(
                    context.InputText(
                        "Timeline Description",
                        newTimelineDescription));
                static_cast<void>(
                    context.InputText(
                        "Timeline Icon",
                        newTimelineIcon));
                if (context.Button(
                        "Add Timeline Mapping") &&
                    !newTimelineEventId.empty())
                {
                    platformConfiguration.
                        steam.timelineEvents.
                        push_back({
                            .eventId =
                                newTimelineEventId,
                            .title =
                                newTimelineTitle,
                            .description =
                                newTimelineDescription,
                            .icon =
                                newTimelineIcon.empty()
                                    ? "steam_marker"
                                    : newTimelineIcon
                        });
                    newTimelineEventId.clear();
                    newTimelineTitle.clear();
                    newTimelineDescription.clear();
                    newTimelineIcon =
                        "steam_marker";
                }

                context.Separator();

                if (context.Button(
                        "Validate Configuration"))
                {
                    if (steamAppIdEditor < 0 ||
                        steamAppIdEditor >
                            4294967295LL)
                    {
                        platformStatus =
                            "Steam App ID is outside the uint32 range";
                    }
                    else
                    {
                        platformConfiguration.
                            steam.appId =
                                static_cast<
                                    orbit::u32>(
                                        steamAppIdEditor);
                        platformConfigurationIssues =
                            orbit::platform_services::
                                ValidatePlatformConfiguration(
                                    platformConfiguration,
                                    platformConfiguration.
                                        steam.enabled);
                        platformStatus =
                            platformConfigurationIssues.
                                    empty()
                                ? "Configuration valid"
                                : std::format(
                                    "{} validation issue{}",
                                    platformConfigurationIssues.
                                        size(),
                                    platformConfigurationIssues.
                                            size() == 1U
                                        ? ""
                                        : "s");
                    }
                }

                context.SameLine();

                if (context.Button(
                        "Save Platform Config"))
                {
                    if (steamAppIdEditor < 0 ||
                        steamAppIdEditor >
                            4294967295LL)
                    {
                        platformStatus =
                            "Steam App ID is outside the uint32 range";
                    }
                    else
                    {
                        platformConfiguration.
                            steam.appId =
                                static_cast<
                                    orbit::u32>(
                                        steamAppIdEditor);
                        orbit::platform_services::
                            SavePlatformConfigurationAtomic(
                                platformConfigurationPath,
                                platformConfiguration);
                        platformConfigurationIssues =
                            orbit::platform_services::
                                ValidatePlatformConfiguration(
                                    platformConfiguration,
                                    platformConfiguration.
                                        steam.enabled);
                        platformStatus =
                            platformConfigurationIssues.
                                    empty()
                                ? "Saved"
                                : std::format(
                                    "Saved with {} validation issue{}",
                                    platformConfigurationIssues.
                                        size(),
                                    platformConfigurationIssues.
                                            size() == 1U
                                        ? ""
                                        : "s");
                    }
                }

                if (context.Button(
                        "Add Shipping Steam Profile"))
                {
                    const auto found =
                        std::ranges::find_if(
                            project.Manifest().
                                buildProfiles,
                            [](const auto& profile)
                            {
                                return profile.storefront ==
                                    "steam";
                            });

                    if (found ==
                        project.Manifest().
                            buildProfiles.end())
                    {
                        project.Manifest().
                            buildProfiles.push_back({
                                .name =
                                    "Shipping Steam",
                                .configuration =
                                    "Shipping",
                                .platform =
                                    "Windows",
                                .storefront =
                                    "steam"
                            });
                        project.Save();
                        platformStatus =
                            "Added Shipping Steam build profile";
                    }
                    else
                    {
                        platformStatus =
                            "Steam build profile already exists";
                    }
                }

                context.Separator();
                context.Text(
                    std::format(
                        "Status: {}",
                        platformStatus));

                for (const auto& issue :
                     platformConfigurationIssues)
                {
                    context.Text(
                        std::format(
                            "[{}] {}",
                            issue.code,
                            issue.message));
                }
            }
    });
}
} // namespace orbit::editor_app
