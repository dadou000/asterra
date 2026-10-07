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

void StudioOutputPanel::Register()
{
    ui.RegisterPanel({
        .id = kOutputPanel,
        .title = "Output",
        .defaultOpen = true,
        .defaultDock = orbit::editor_ui::DockRegion::Bottom,
        .dockOrder = 0,
        .draw =
            [this](
                orbit::editor_ui::
                    PanelContext& context)
            {
                if (context.Button("Clear"))
                {
                    outputLog.Clear();
                }

                context.Separator();

                for (const auto& entry :
                     outputLog.Snapshot())
                {
                    context.Text(
                        std::format(
                            "[{}] {}",
                            LogPrefix(
                                entry.level),
                            entry.message));
                }
            }
    });
}
} // namespace orbit::editor_app
