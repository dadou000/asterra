#include "StudioWorkspaceRpc.hpp"
#include "StudioApplication.hpp"
#include <orbit/build/BuildService.hpp>
#include <orbit/commands/CommandRegistry.hpp>
#include <orbit/commands/CommandService.hpp>
#include <orbit/content/ContentService.hpp>
#include <orbit/content/RuntimeTexture.hpp>
#include <orbit/content_wic/WicTextureImporter.hpp>
#include <orbit/core/Log.hpp>
#include <orbit/core/ThreadName.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/dev_server/DevServer.hpp>
#include <orbit/shading/ShaderPreviewRenderer.hpp>
#include <orbit/shading/ShadingCapture.hpp>
#include <orbit/shading/ShadingRpc.hpp>
#include <orbit/shading/ShadingWorkspace.hpp>
#include <orbit/shading/MaterialThumbnailCache.hpp>
#include <orbit/studio_ui/ShadingUi.hpp>
#include <orbit/studio_ui/StudioTextDiagnosticsHud.hpp>
#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/BuiltinSchemas.hpp>
#include <orbit/editor_model/CommandSurfaces.hpp>
#include <orbit/editor_model/ExplorerModel.hpp>
#include <orbit/editor_model/InspectorModel.hpp>
#include <orbit/editor_model/OutputLog.hpp>
#include <orbit/editor_model/ShortcutRegistry.hpp>
#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/editor_rpc/EditorRpcService.hpp>
#include <orbit/editor_ui/BodyPreviewRenderer.hpp>
#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/frames/FrameGraph.hpp>
#include <orbit/jobs/JobSystem.hpp>
#include <orbit/editor_rpc/ProfilerRpc.hpp>
#include <orbit/studio_ui/StudioFlatMapRpc.hpp>
#include <orbit/profiler/Profiler.hpp>
#include <orbit/render_graph/GpuPassTimer.hpp>
#include <orbit/lighting/LightingScheduler.hpp>
#include <orbit/lighting/MaterialEmission.hpp>
#include <orbit/path_geometry/PathDerived.hpp>
#include <orbit/path_geometry/PathSource.hpp>
#include <orbit/path_routing/RouteDomains.hpp>
#include <orbit/path_routing/RoutePlanner.hpp>
#include <orbit/platform/FileBrowser.hpp>
#include <orbit/platform/FileDialog.hpp>
#include <orbit/platform/Paths.hpp>
#include <orbit/platform_services/PlatformConfig.hpp>
#include <orbit/paths/PathNetwork.hpp>
#include <orbit/plugins/PluginManager.hpp>
#include <orbit/render_graph/RenderGraph.hpp>
#include <orbit/render_view/Capture.hpp>
#include <orbit/render_view/RenderView.hpp>
#include <orbit/rhi/vulkan/VulkanBackend.hpp>
#include <orbit/runtime/RuntimeSession.hpp>
#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/selection/SelectionService.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/studio_session/StudioWorkspace.hpp>
#include <orbit/studio_session/StudioRuntimeBinding.hpp>
#include <orbit/studio_session/StudioTerrainRoundTripVerifier.hpp>
#include <orbit/studio_ui/CelestialAuthoringUi.hpp>
#include <orbit/studio_ui/DebugViewUi.hpp>
#include <orbit/studio_ui/ProfilerUi.hpp>
#include <orbit/studio_ui/ImplementationPlan.hpp>
#include <orbit/studio_ui/PlanningUi.hpp>
#include <orbit/studio_ui/ReportThumbnailCache.hpp>
#include <orbit/studio_ui/ReportsUi.hpp>
#include <orbit/studio_ui/SimulationControlsUi.hpp>
#include <orbit/studio_ui/ViewportCaptureService.hpp>
#include <orbit/studio_ui/DisplayDiagnosticsUi.hpp>
#include <orbit/studio_ui/DisplayEyeRpc.hpp>
#include <orbit/studio_ui/ProjectAuthoringUi.hpp>
#include <orbit/studio_ui/ProjectSettingsUi.hpp>
#include <orbit/studio_ui/StudioRenderViewRpc.hpp>
#include <orbit/studio_ui/StudioRenderViewSet.hpp>
#include <orbit/studio_ui/StudioViewContinuity.hpp>
#include <orbit/studio_ui/StudioViewportPanels.hpp>
#include <orbit/studio_ui/StudioViewportRenderer.hpp>
#include <orbit/studio_ui/SurfaceAuthoringUi.hpp>
#include <orbit/studio_ui/SystemViewUi.hpp>
#include <orbit/studio_ui/VolumeAuthoringUi.hpp>
#include <orbit/studio_ui/WorldDocumentsUi.hpp>
#include <orbit/universe/BodyRegistry.hpp>
#include <orbit/universe/ReferenceSurface.hpp>
#include <orbit/world_model/MaterialAssignmentBinding.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>
#include <orbit/volume_fields/VolumeFieldStorage.hpp>
#include <orbit/volume_solver/SurfaceVolumeSolver.hpp>
#include "EditorAppSupport.hpp"
#include "StudioBuildHost.hpp"
#include "StudioPanels.hpp"
#include "StudioPathNetworkHost.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace orbit::editor_app
{
[[nodiscard]] bool RelaunchStudioWithProject(
    const std::filesystem::path& projectManifest);

namespace
{
[[nodiscard]] orbit::u16 RpcPortFromEnvironment()
{
    constexpr unsigned int defaultPort = 4320;
    std::array<char, 16> environmentValue{};
    const char* configuredPort = nullptr;
    bool hasConfiguredPort = false;
#if defined(_MSC_VER)
    std::size_t environmentSize = 0;
    if (getenv_s(
            &environmentSize,
            environmentValue.data(),
            environmentValue.size(),
            "ORBIT_RPC_PORT") != 0)
    {
        throw std::invalid_argument(
            "ORBIT_RPC_PORT could not be read.");
    }
    configuredPort = environmentValue.data();
    hasConfiguredPort = environmentSize > 1;
#else
    configuredPort = std::getenv("ORBIT_RPC_PORT");
    hasConfiguredPort =
        configuredPort != nullptr &&
        *configuredPort != '\0';
#endif

    if (!hasConfiguredPort)
    {
        return static_cast<orbit::u16>(defaultPort);
    }

    unsigned int port = 0;
    const std::string_view value(configuredPort);
    const auto [end, error] = std::from_chars(
        value.data(),
        value.data() + value.size(),
        port);

    if (error != std::errc{} ||
        end != value.data() + value.size() ||
        port == 0 ||
        port > 65535)
    {
        throw std::invalid_argument(
            "ORBIT_RPC_PORT must be an integer from 1 to 65535.");
    }

    return static_cast<orbit::u16>(port);
}
} // namespace
}

using namespace orbit::editor_app::support;
using namespace orbit::editor_app;

// The process entry point is main() in HotReloadBootstrap.cpp, which starts hot-reload after C++ global
// initialisation has completed and then calls this.
int orbit::editor_app::StudioApplication::Run(
    const int argc,
    char** argv)
{
    orbit::core::SetCurrentThreadName("Orbit.Main");

    // Always-on micro-profiler: per-thread scope rings plus a hitch/stall
    // watchdog on this (the frame-driving) thread. See docs/ORBIT_PROFILER.md.
    orbit::profiler::StartWatchdog();
    struct ProfilerShutdown
    {
        ~ProfilerShutdown()
        {
            orbit::profiler::StopWatchdog();
        }
    } profilerShutdown;

    try
    {
        orbit::editor_model::OutputLog
            outputLog;

        bool terrainUiSmoke = false;

        for (int index = 1;
             index < argc;
             ++index)
        {
            if (argv[index] != nullptr &&
                std::string_view(argv[index]) ==
                    "--terrain-ui-smoke")
            {
                terrainUiSmoke = true;
                break;
            }
        }

        // One application window for the whole session: the project browser
        // runs inside it before the project is opened, then the editor takes
        // over the same window/device.
        orbit::runtime::RuntimeSession runtime({
            .applicationName = "OrbitStudio",
            .windowTitle = "Orbit Studio",
            .startMaximized = true,
            .width = 1680,
            .height = 980,
            .swapchainBufferCount = 3,
            .allowTearing = true,
            .relativeMouseMode = false
        });

        std::filesystem::path
            terrainUiSmokeRoot;

        std::optional<
            orbit::documents::ProjectDocument>
            openedProject;

        if (terrainUiSmoke)
        {
            terrainUiSmokeRoot =
                std::filesystem::
                    temp_directory_path() /
                ("orbit-terrain-ui-smoke-" +
                 orbit::documents::ProjectId::
                     Random().
                     ToString());

            openedProject =
                orbit::documents::
                    ProjectDocument::Create(
                        terrainUiSmokeRoot,
                        "Orbit Terrain UI Smoke");
        }
        else
        {
            openedProject =
                OpenProject(
                    argc,
                    argv);
        }

        if (!openedProject.has_value())
        {
            return 0;
        }

        orbit::documents::ProjectDocument
            project =
                std::move(
                    *openedProject);

        // The legacy Studio shell now consumes the same permanent world-scoped
        // service graph as the M20A application model. This removes the second
        // WorldDatabase/ObjectStore/command/plugin stack that previously lived
        // beside StudioSession and makes semantic world authority singular.
        orbit::studio_session::StudioSession
            studioSession(project);

        auto& worldSession =
            studioSession.World();

        const auto world =
            [&worldSession]()
                -> orbit::documents::WorldDatabase&
            {
                return worldSession.World();
            };
        const auto schemas =
            [&worldSession]()
                -> orbit::schema::SchemaRegistry&
            {
                return worldSession.Schemas();
            };
        const auto objects =
            [&worldSession]()
                -> orbit::scene::ObjectStore&
            {
                return worldSession.Objects();
            };
        const auto selection =
            [&worldSession]()
                -> orbit::selection::SelectionService&
            {
                return worldSession.Selection();
            };
        const auto commandService =
            [&worldSession]()
                -> orbit::commands::CommandService&
            {
                return worldSession.Commands();
            };
        const auto authoringCommands =
            [&worldSession]()
                -> orbit::commands::CommandRegistry&
            {
                return worldSession.CommandRegistry();
            };
        const auto commandSurfaces =
            [&worldSession]()
                -> orbit::editor_model::
                    CommandSurfaceRegistry&
            {
                return worldSession.CommandSurfaces();
            };
        const auto explorer =
            [&worldSession]()
                -> orbit::editor_model::ExplorerModel&
            {
                return worldSession.Explorer();
            };
        const auto inspector =
            [&worldSession]()
                -> orbit::editor_model::InspectorModel&
            {
                return worldSession.Inspector();
            };
        const std::function<
            orbit::plugins::PluginManager&()>
            plugins =
                [&worldSession]()
                    -> orbit::plugins::PluginManager&
                {
                    return worldSession.Plugins();
                };

        if (terrainUiSmoke)
        {
            if (!worldSession.HasWorld())
            {
                const auto smokeWorld =
                    studioSession.CreateWorld(
                        "Worlds/TerrainUiSmoke.orbitworld",
                        "Terrain UI Smoke");

                studioSession.OpenWorld(
                    smokeWorld.relativePath);
            }

            if (!FindFirstBodyObject(
                    objects()).
                    has_value())
            {
                const auto worldObject =
                    commandService().
                        CreateObject(
                            orbit::editor_model::
                                builtin::kWorldType,
                            "World");

                const orbit::scene::ObjectId
                    selectedWorld[]{
                        worldObject
                    };

                selection().Set(
                    selectedWorld);

                authoringCommands().Invoke(
                    orbit::editor_model::
                        authoring_commands::
                            kCreateRockyPlanet,
                    {
                        {
                            "name",
                            std::string(
                                "Terrain UI Smoke Planet")
                        },
                        {
                            "radiusMeters",
                            6'000'000.0
                        },
                        {
                            "massKg",
                            5.0e24
                        }
                    });
            }
        }

        orbit::content::ContentService
            content(
                project.RootDirectory());

        orbit::content_wic::
            RegisterTextureImporters(
                content.Importers());

        content.Scan();

        for (const auto& diagnostic :
             content.Diagnostics())
        {
            orbit::log::Warning(
                std::format(
                    "Content '{}': {}",
                    diagnostic.sourcePath.
                        generic_string(),
                    diagnostic.message));
        }

        const std::filesystem::path
            scratchPreviewRoot =
                orbit::platform::
                    UserDataDirectory() /
                "Scratch" /
                "StudioPreview";

        const bool isScratchPreview =
            std::filesystem::absolute(
                project.RootDirectory()).
                lexically_normal() ==
            std::filesystem::absolute(
                scratchPreviewRoot).
                lexically_normal();

        orbit::scene::ObjectId bodyObject{};

        if (FindFirstBodyObject(objects()).has_value() ||
            isScratchPreview)
        {
            bodyObject =
                EnsureInitialBodyObject(
                    objects(),
                    commandService());
        }

        if (bodyObject.IsValid())
        {
            const std::array initialSelection{
                bodyObject
            };

            selection().Set(
                std::span(
                    initialSelection));
        }

        // Real user projects may intentionally be blank. Composition remains
        // valid with zero bodies; only the built-in scratch preview bootstraps
        // a default semantic system/body.
        static_cast<void>(
            worldSession.RebuildUniverse());

        auto& rpcHost =
            studioSession.Rpc();

        CpuFrameTelemetry cpuFrameTelemetry;
        rpcHost.Dispatcher().Register(
            {
                .name = "studio.cpu_timings",
                .description =
                    "Read last and rolling 120-frame CPU timings for the editor loop.",
                .mutating = false
            },
            [&cpuFrameTelemetry](const orbit::rpc::Value&)
            {
                return cpuFrameTelemetry.Snapshot();
            });

        orbit::editor_rpc::RegisterProfilerRpc(
            rpcHost.Dispatcher());

        orbit::dev_server::DevServer
            rpcServer({
                .port = RpcPortFromEnvironment(),
                .maxMessageBytes =
                    1024U * 1024U
            });

        // Filled from the RPC handler and drained into the UI each frame; both
        // run on the main thread (DevServer::Poll is frame-driven).
        std::vector<orbit::editor_ui::EditorUi::Notification>
            pendingRpcNotifications;

        rpcServer.SetMessageHandler(
            [&studioSession,
             &pendingRpcNotifications](
                const std::string_view message)
            {
                auto response =
                    studioSession.DispatchRpc(
                        message);
                RecordRpcNotifications(
                    studioSession.Rpc().Dispatcher(),
                    message,
                    response,
                    pendingRpcNotifications);
                return response;
            });

        commandSurfaces().Set(
            "viewport",
            orbit::editor_model::
                CommandSurfaceKind::Toolbar,
            {
                orbit::editor_model::
                    authoring_commands::kUndo,
                orbit::editor_model::
                    authoring_commands::kRedo,
                orbit::editor_model::
                    authoring_commands::
                        kClearSelection,
                orbit::editor_model::
                    authoring_commands::
                        kConnectPathDirect,
                orbit::editor_model::
                    authoring_commands::
                        kConnectPathBezier,
                orbit::editor_model::
                    authoring_commands::
                        kConnectPathRouted
            });

        commandSurfaces().Set(
            "viewport",
            orbit::editor_model::
                CommandSurfaceKind::Radial,
            {
                orbit::editor_model::
                    authoring_commands::
                        kMoveToRoot,
                orbit::editor_model::
                    authoring_commands::
                        kClearSelection,
                orbit::editor_model::
                    authoring_commands::
                        kConnectPathDirect,
                orbit::editor_model::
                    authoring_commands::
                        kConnectPathBezier,
                orbit::editor_model::
                    authoring_commands::
                        kConnectPathRouted,
                orbit::editor_model::
                    authoring_commands::kUndo,
                orbit::editor_model::
                    authoring_commands::kRedo
            });

        commandSurfaces().Set(
            "explorer",
            orbit::editor_model::
                CommandSurfaceKind::
                    ContextMenu,
            {
                orbit::editor_model::
                    authoring_commands::
                        kMoveToRoot,
                orbit::editor_model::
                    authoring_commands::
                        kClearSelection,
                orbit::editor_model::
                    authoring_commands::
                        kConnectPathDirect,
                orbit::editor_model::
                    authoring_commands::
                        kConnectPathBezier,
                orbit::editor_model::
                    authoring_commands::
                        kConnectPathRouted,
                orbit::editor_model::
                    authoring_commands::kUndo,
                orbit::editor_model::
                    authoring_commands::kRedo
            });

        orbit::editor_model::ShortcutRegistry
            shortcuts;

        shortcuts.Register(
            {
                .key =
                    orbit::platform::Key::Z,
                .control = true
            },
            orbit::editor_model::
                authoring_commands::kUndo);

        shortcuts.Register(
            {
                .key =
                    orbit::platform::Key::Y,
                .control = true
            },
            orbit::editor_model::
                authoring_commands::kRedo);

        const auto presentActions =
            [&commandSurfaces,
             &authoringCommands,
             &worldSession](
                const std::string_view surface,
                const orbit::editor_model::
                    CommandSurfaceKind kind)
            {
                std::vector<
                    orbit::editor_ui::
                        ActionPresentation>
                    result;

                if (!worldSession.HasWorld())
                {
                    return result;
                }

                for (const auto& command :
                     commandSurfaces().Present(
                         surface,
                         kind,
                         authoringCommands()))
                {
                    const orbit::commands::
                        CommandId id =
                            command.id;

                    result.push_back({
                        .label = command.label,
                        .enabled =
                            command.enabled,
                        .disabledReason =
                            command.
                                disabledReason,
                        .invoke =
                            [&authoringCommands,
                             id]
                            {
                                try
                                {
                                    authoringCommands().
                                        Invoke(id);
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
                    });
                }

                return result;
            };

        const std::string windowTitle =
            std::format(
                "Orbit Studio - {}",
                project.Manifest().
                    displayName);

        runtime.Window().SetTitle(windowTitle);

        orbit::platform::Window& window =
            runtime.Window();
        std::atomic_bool ecoMode{false};
        rpcHost.SetActivityCallback([&window] { window.WakeForActivity(); });
        rpcHost.Dispatcher().Register(
            {.name = "studio.eco_mode_get",
             .description = "Read whether Eco mode is enabled.",
             .mutating = false},
            [&ecoMode](const orbit::rpc::Value&)
            {
                return orbit::rpc::Value::Object{
                    {"enabled", ecoMode.load(std::memory_order_relaxed)}};
            });
        rpcHost.Dispatcher().Register(
            {.name = "studio.eco_mode_set",
             .description = "Enable or disable Eco mode.",
             .mutating = true},
            [&ecoMode](const orbit::rpc::Value& params)
            {
                const auto* enabled = params.Find("enabled");
                if (enabled == nullptr || !enabled->IsBool())
                {
                    throw std::invalid_argument("enabled must be a boolean");
                }
                ecoMode.store(enabled->AsBool(), std::memory_order_relaxed);
                return orbit::rpc::Value::Object{
                    {"enabled", enabled->AsBool()}};
            });
        orbit::rhi::Device& device =
            runtime.Device();
        rpcHost.Dispatcher().Register(
            {
                .name = "renderdoc.status",
                .description =
                    "RenderDoc availability (needs Studio launched with ORBIT_RENDERDOC=1 "
                    "and RenderDoc installed), whether a capture is in progress, and the "
                    "path of the last completed .rdc capture.",
                .mutating = false
            },
            [&device](const orbit::rpc::Value&)
            {
                return orbit::rpc::Value(orbit::rpc::Value::Object{
                    {"available", orbit::rhi::vulkan::IsRenderDocAvailable(device)},
                    {"capturing", orbit::rhi::vulkan::IsRenderDocCapturing(device)},
                    {"last_capture_path", orbit::rhi::vulkan::LastRenderDocCapturePath(device)}});
            });
        rpcHost.Dispatcher().Register(
            {
                .name = "renderdoc.capture",
                .description =
                    "Captures the next presented frame with RenderDoc. Poll renderdoc.status "
                    "until capturing is false and last_capture_path changes, then open the "
                    ".rdc in the RenderDoc UI. Fails when RenderDoc is not available.",
                .mutating = true
            },
            [&device](const orbit::rpc::Value&)
            {
                if (!orbit::rhi::vulkan::IsRenderDocAvailable(device))
                {
                    throw orbit::rpc::Error(
                        1081,
                        "RenderDoc is not available: launch Studio with ORBIT_RENDERDOC=1 and RenderDoc installed.");
                }
                orbit::rhi::vulkan::TriggerRenderDocCapture(device);
                return orbit::rpc::Value(orbit::rpc::Value::Object{
                    {"requested", true},
                    {"previous_capture_path", orbit::rhi::vulkan::LastRenderDocCapturePath(device)}});
            });

        orbit::rhi::Queue& graphicsQueue =
            runtime.GraphicsQueue();
        orbit::rhi::Swapchain& swapchain =
            runtime.Swapchain();

        const orbit::shader::dxc::
            DxcShaderCompiler compiler;

        // The visible legacy preview now consumes UniverseComposition,
        // the same semantic-to-runtime derivation used by StudioSession.
        // No editor-only Helion/Asterra FrameGraph or BodyRegistry is built.
        auto& universe =
            worldSession.Universe();

        const auto frames =
            [&worldSession]()
                -> orbit::frames::FrameGraph&
            {
                return worldSession.
                    Universe().Frames();
            };

        const auto bodies =
            [&worldSession]()
                -> orbit::universe::BodyRegistry&
            {
                return worldSession.
                    Universe().Bodies();
            };

        const auto composedBodyId =
            bodyObject.IsValid()
                ? universe.BodyForObject(
                      bodyObject)
                : std::nullopt;

        if (bodyObject.IsValid() &&
            (!composedBodyId.has_value() ||
             bodies().FindBody(*composedBodyId) ==
                 nullptr))
        {
            throw std::runtime_error(
                "Studio failed to compose its authored semantic body.");
        }

        orbit::universe::BodyId
            bodyId =
                composedBodyId.value_or(
                    orbit::universe::BodyId{});

        orbit::render_view::RenderView
            materialView(
                device,
                {
                    .width = 384,
                    .height = 240
                });

        if (const auto* initialBody =
                bodyId.IsValid()
                    ? bodies().FindBody(bodyId)
                    : nullptr;
            initialBody != nullptr)
        {
            materialView.Camera().frame =
                initialBody->frame;
        }
        else
        {
            materialView.Camera().frame = {};
        }

        materialView.Camera().localPositionMeters = {
            0.0,
            0.0,
            -3.2
        };
        materialView.Camera().nearPlaneMeters = 0.01F;
        materialView.Camera().farPlaneMeters = 10.0F;
        materialView.Camera().forward = {
            0.0F,
            0.0F,
            1.0F
        };
        materialView.Camera().up = {
            0.0F,
            1.0F,
            0.0F
        };

        orbit::editor_ui::
            BodyPreviewRenderer
                bodyPreview(
                    device,
                    compiler);

        // Shading tab. The workspace is the model that both the panel and the
        // shading.* RPC/MCP methods drive; the view and renderer draw the
        // preview. A shader edit reaches the renderer as a new pipeline at a
        // frame boundary - Studio is never restarted for it.
        orbit::shading::ShadingWorkspace
            shadingWorkspace(
                content,
                &compiler);
        orbit::render_view::RenderView
            shadingView(
                device,
                {
                    .width = 640,
                    .height = 384
                });
        orbit::shading::ShaderPreviewRenderer
            shadingRenderer(
                device,
                compiler,
                graphicsQueue);
        // Small static sphere-preview thumbnails for the Shading tree's
        // rows, rendered on demand with its own dedicated
        // ShaderPreviewRenderer/targets -- independent of whatever is
        // currently open in shadingWorkspace/shadingRenderer above.
        orbit::shading::MaterialThumbnailCache
            shadingThumbnails(
                device,
                compiler,
                graphicsQueue,
                content);
        bool shadingRenderedOnce = false;

        // Smoke runs use a throwaway project, so their layout must not
        // accumulate in the user's real EditorLayouts directory (and must
        // always start from the default arrangement).
        const std::filesystem::path
            layoutPath =
                terrainUiSmoke
                    ? terrainUiSmokeRoot /
                          "EditorLayout.ini"
                    : orbit::platform::
                              UserDataDirectory() /
                          "EditorLayouts" /
                          (project.Manifest().
                               projectId.ToString() +
                           ".ini");

        // In-editor Project Browser. Opening/creating a project here hands the
        // choice to a fresh Studio process (project-bound state is rebuilt).
        orbit::studio_session::StudioWorkspace
            projectBrowserWorkspace;
        orbit::studio_ui::ProjectAuthoringUi
            projectBrowserUi(
                projectBrowserWorkspace,
                orbit::platform::
                    UserDataDirectory() /
                    "RecentProjects.txt");
        std::optional<std::filesystem::path>
            pendingProjectSwitch;
        projectBrowserUi.
            SetWorkspaceChangedCallback(
                [&]()
                {
                    if (projectBrowserWorkspace.
                            HasProject())
                    {
                        projectBrowserWorkspace.
                            Project().Save();
                        projectBrowserWorkspace.
                            Session().World().
                            Checkpoint();
                        pendingProjectSwitch =
                            projectBrowserWorkspace.
                                Project().
                                ManifestPath();
                    }
                });

        RegisterStudioProjectRpc(rpcHost.Dispatcher(), projectBrowserUi);

        orbit::editor_ui::EditorUi ui(
            device,
            graphicsQueue,
            compiler,
            layoutPath,
            window.DpiScale());

        projectBrowserUi.SetDialogOwner(&window);
        projectBrowserUi.RegisterProjectBrowser(ui);

        // Validation seam: `--automation-open-menu=View` holds that dropdown
        // open so screenshots/inspection need no real mouse input.
        for (int index = 1;
             index < argc;
             ++index)
        {
            constexpr std::string_view kOpenMenu =
                "--automation-open-menu=";

            const std::string_view argument =
                argv[index] != nullptr
                    ? std::string_view(argv[index])
                    : std::string_view();

            if (argument.starts_with(kOpenMenu))
            {
                ui.SetAutomationOpenMenu(
                    std::string(
                        argument.substr(
                            kOpenMenu.size())));
            }
        }

        orbit::studio_session::StudioRuntimeBinding
            studioRuntime(
                studioSession);

        orbit::studio_ui::StudioRenderViewSet
            studioViews(
                device,
                studioSession);

        // Remembers the primary camera and simulation time per project and
        // restores them on open. Declared after studioViews/studioSession so
        // its final save runs while both are still alive.
        orbit::studio_ui::StudioViewContinuity
            viewContinuity;

        // Text diagnostics readout over the primary viewport image.
        orbit::studio_ui::StudioTextDiagnosticsHud
            primaryTextHud;

        orbit::studio_ui::StudioViewportPanels
            studioViewportPanels(
                studioViews,
                studioSession);
        studioViewportPanels.SetContentService(&content);
        studioViewportPanels.SetEcoModeAccessors(
            [&ecoMode] { return ecoMode.load(std::memory_order_relaxed); },
            [&ecoMode](const bool enabled)
            {
                ecoMode.store(enabled, std::memory_order_relaxed);
            });

        shortcuts.RegisterCallback(
            {
                .key = orbit::platform::Key::P,
                .control = true,
                .shift = true
            },
            [&studioViewportPanels]
            {
                studioViewportPanels.
                    RequestCommandPaletteOpen();
            },
            true);

        orbit::volume_fields::
            VolumeFieldStorageService
                volumeFieldStorage(
                    device);

        orbit::volume_solver::
            SurfaceVolumeSolverService
                surfaceVolumeSolver(
                    device,
                    compiler,
                    swapchain.BufferCount());

        orbit::studio_ui::StudioViewportRenderer
            studioViewportRenderer(
                device,
                compiler,
                swapchain.BufferCount());

        studioViewportRenderer.
            SetContentService(
                &content);
        studioViewportRenderer.
            SetVolumeFieldStorageService(
                &volumeFieldStorage);
        studioViewportRenderer.
            SetSurfaceVolumeSolverService(
                &surfaceVolumeSolver);

        orbit::studio_ui::DisplayDiagnosticsUi
            displayDiagnosticsUi(
                studioViewportRenderer);
        displayDiagnosticsUi.Register(ui);
        orbit::studio_ui::RegisterDisplayEyeRpc(
            rpcHost.Dispatcher(),
            studioViewportRenderer);

        orbit::studio_ui::DebugViewUi
            debugViewUi(
                studioViews);
        debugViewUi.Register(ui);

        // CPU profiler panel + profiler.panel_* / profiler.snapshot RPC, both
        // driving one model (docs/ORBIT_PROFILER.md).
        orbit::studio_ui::ProfilerUi profilerUi;
        profilerUi.Register(ui);
        orbit::studio_ui::RegisterProfilerPanelRpc(
            rpcHost.Dispatcher(),
            profilerUi.Model());

        // Simulation transport (Simulate / Pause / Step) and time.* RPC, both
        // driving the one Studio clock that moves the planets, the sun and the
        // atmosphere (docs/ORBIT_MCP.md).
        orbit::studio_ui::SimulationControls simulationControls(
            studioSession.Clock());
        orbit::studio_ui::SimulationControlsUi simulationUi(
            simulationControls,
            studioSession);
        simulationUi.Register(ui);
        orbit::studio_ui::RegisterSimulationRpc(
            rpcHost.Dispatcher(),
            simulationControls,
            studioSession);

        // Issue reports: a Reports panel and reports.* RPC over one store,
        // saved with the project so a report survives a crash.
        orbit::studio_reports::ReportStore reportStore;
        try
        {
            reportStore.Open(
                project.RootDirectory() / "Reports" / "reports.json");
        }
        catch (const std::exception& exception)
        {
            // Left unopened so a damaged file is never overwritten.
            orbit::log::Warning(
                std::format(
                    "Reports: cannot open the report file, reports will not "
                    "be saved: {}",
                    exception.what()));
        }
        orbit::studio_ui::ReportsController reportsController(
            reportStore,
            rpcHost.Dispatcher());
        orbit::studio_ui::ReportsUi reportsUi(reportsController);
        reportsUi.Register(ui);
        orbit::studio_ui::RegisterReportsRpc(
            rpcHost.Dispatcher(),
            reportsController);
        orbit::studio_ui::RegisterReportsUiRpc(
            rpcHost.Dispatcher(),
            reportsUi);
        simulationUi.SetReportIssueHandler(
            [&reportsUi]
            {
                reportsUi.NewReportAndShow();
            });

        // The implementation plan is project-local and shared by the planning
        // canvas and planning.* RPC/MCP operations.
        orbit::studio_ui::ImplementationPlan implementationPlan;
        try
        {
            implementationPlan.Open(
                project.RootDirectory() / "Planning" / "implementation-plan.json");
        }
        catch (const std::exception& exception)
        {
            orbit::log::Warning(
                std::format(
                    "Planning: cannot open the plan file; changes will not be "
                    "saved this session: {}",
                    exception.what()));
        }
        orbit::studio_ui::PlanningUi planningUi(implementationPlan);
        planningUi.Register(ui);
        planningUi.RegisterRpc(rpcHost.Dispatcher());

        orbit::studio_ui::RegisterStudioRenderViewRpc(
            rpcHost.Dispatcher(),
            studioViews);
        orbit::studio_ui::RegisterStudioFlatMapRpc(
            rpcHost.Dispatcher(),
            studioViews);

        orbit::lighting::LightingScheduler
            lightingScheduler;

        orbit::lighting::LightingTimestampRecorder
            lightingTimestamps(
                device,
                swapchain.BufferCount());

        // GPU time per render-graph pass, shown on the profiler's "GPU passes"
        // lane (docs/ORBIT_PROFILER.md).
        orbit::render_graph::GpuPassTimer
            gpuPassTimer(
                device,
                swapchain.BufferCount());

        auto* primaryStudioView =
            studioViews.Find(
                "studio.primary");

        if (primaryStudioView == nullptr)
        {
            throw std::logic_error(
                "Studio primary RenderView was not created.");
        }
        std::optional<std::pair<orbit::u32, orbit::u32>>
            pendingPrimaryViewResize;
        std::optional<std::pair<orbit::u32, orbit::u32>>
            pendingMaterialViewResize;

        // Fullscreen and 16K captures of the primary viewport: resize, settle,
        // capture, restore. Driven once per frame by the loop below.
        orbit::studio_ui::ViewportCaptureService viewportCapture(
            {
                .viewSize =
                    [primaryStudioView]()
                    {
                        return std::pair{
                            primaryStudioView->Width(),
                            primaryStudioView->Height()};
                    },
                .windowSize =
                    [&window]()
                    {
                        return std::pair{
                            window.Width(),
                            window.Height()};
                    },
                .capture =
                    [&device,
                     &graphicsQueue,
                     primaryStudioView](
                        const std::filesystem::path& path)
                    {
                        return orbit::render_view::CaptureImageFile(
                            device,
                            graphicsQueue,
                            *primaryStudioView,
                            path);
                    },
                .viewFovRadians =
                    [&studioViews]()
                    {
                        return studioViews.ViewFovRadians(
                            "studio.primary");
                    },
                .captureImage =
                    [&device,
                     &graphicsQueue,
                     primaryStudioView]()
                    {
                        return orbit::render_view::CaptureRgba8(
                            device,
                            graphicsQueue,
                            *primaryStudioView);
                    },
                .setTile =
                    [&studioViews](
                        const std::optional<
                            orbit::studio_ui::ViewportCaptureService::Tile>&
                                tile)
                    {
                        if (!tile.has_value())
                        {
                            studioViews.SetCaptureTile(
                                "studio.primary",
                                std::nullopt);
                            return;
                        }
                        studioViews.SetCaptureTile(
                            "studio.primary",
                            orbit::studio_ui::StudioCaptureTile{
                                .yawRadians = tile->yawRadians,
                                .pitchRadians = tile->pitchRadians,
                                .verticalFovRadians =
                                    tile->verticalFovRadians});
                    },
                .lockExposure =
                    [&studioViewportRenderer](const bool locked)
                    {
                        studioViewportRenderer.
                            SetHumanEyeAdaptationLocked(
                                "studio.primary",
                                locked);
                    },
                .simulationPlaying =
                    [&simulationControls]()
                    {
                        return simulationControls.Playing();
                    },
                .setSimulationPlaying =
                    [&simulationControls](const bool playing)
                    {
                        simulationControls.SetPlaying(playing);
                    },
                .busy =
                    [&studioViewportRenderer]()
                    {
                        return studioViewportRenderer.
                            HasPendingTerrainWork();
                    },
                .openPath =
                    [](const std::filesystem::path& path)
                    {
                        orbit::platform::OpenInFileBrowser(path);
                    }
            },
            project.RootDirectory() / "Screenshots");
        orbit::studio_ui::RegisterViewportCaptureRpc(
            rpcHost.Dispatcher(),
            viewportCapture);

        // Reports carry a screenshot of the viewport (PNG next to the reports
        // file) and the panel shows it, so a problem can be looked at without
        // launching anything. Not while a high-resolution capture has the view
        // resized.
        reportsController.SetScreenshotHook(
            [&device,
             &graphicsQueue,
             primaryStudioView,
             &viewportCapture](
                const std::filesystem::path& path)
            {
                if (viewportCapture.Active())
                {
                    return false;
                }
                static_cast<void>(
                    orbit::render_view::CaptureImageFile(
                        device,
                        graphicsQueue,
                        *primaryStudioView,
                        path));
                return true;
            });
        orbit::studio_ui::ReportThumbnailCache reportThumbnails(
            device,
            graphicsQueue);
        reportsUi.SetThumbnails(&reportThumbnails);
        reportsController.SetOpenHook(
            [](const std::filesystem::path& path)
            {
                orbit::platform::OpenInFileBrowser(path);
            });

        rpcHost.AttachViewport({
            .view = primaryStudioView,
            .capture =
                [&device,
                 &graphicsQueue,
                 primaryStudioView](
                    const std::filesystem::path&
                        path)
                {
                    return orbit::render_view::
                        CaptureImageFile(
                            device,
                            graphicsQueue,
                            *primaryStudioView,
                            path);
                },
            .captureBuffer =
                [&device,
                 &graphicsQueue,
                 primaryStudioView](
                    const orbit::render_view::
                        CaptureBuffer buffer,
                    const std::filesystem::path&
                        path)
                {
                    return orbit::render_view::
                        CaptureFloatBuffer(
                            device,
                            graphicsQueue,
                            *primaryStudioView,
                            buffer,
                            path);
                },
            .focusBody =
                [&studioViews]()
                {
                    return studioViews.
                        FocusTerrainBody(
                            "studio.primary");
                }
        });

        studioViewportPanels.
            RegisterSecondary(ui);

        orbit::studio_ui::ShadingUi
            shadingUi(
                shadingWorkspace,
                [&shadingView]()
                    -> orbit::rhi::Texture*
                {
                    return &shadingView.Color();
                },
                [&shadingThumbnails](
                    const std::filesystem::path& path)
                    -> orbit::rhi::Texture*
                {
                    return shadingThumbnails.Get(path);
                });
        shadingUi.Register(ui);

        RegisterStudioWorkspaceRpc(rpcHost.Dispatcher(), ui, studioViews, studioSession, studioViewportPanels);

        orbit::shading::RegisterShadingRpc(
            rpcHost.Dispatcher(),
            shadingWorkspace,
            {
                .screenshot =
                    [&device,
                     &graphicsQueue,
                     &shadingView,
                     &shadingRenderedOnce](
                        const std::filesystem::path& path)
                    {
                        if (!shadingRenderedOnce)
                        {
                            throw std::runtime_error(
                                "The Shading preview has not rendered yet: open the Shading tab or select a shader first.");
                        }

                        // The preview target is float scene colour that the
                        // shading wrapper has already display-encoded; dump
                        // it exactly and convert to a BMP.
                        auto dump = path;
                        dump += ".ofb";

                        const auto captured =
                            orbit::render_view::
                                CaptureFloatBuffer(
                                    device,
                                    graphicsQueue,
                                    shadingView,
                                    orbit::render_view::
                                        CaptureBuffer::
                                            SceneColor,
                                    dump);
                        const auto image =
                            orbit::shading::
                                ReadFloatCapture(
                                    captured.path);
                        std::filesystem::remove(
                            captured.path);
                        orbit::shading::WriteBmp32(
                            path,
                            image);

                        return orbit::rpc::Value(
                            orbit::rpc::Value::Object{
                                {"path", path.generic_string()},
                                {"width",
                                 static_cast<orbit::i64>(
                                     image.width)},
                                {"height",
                                 static_cast<orbit::i64>(
                                     image.height)},
                                {"file_bytes",
                                 static_cast<orbit::i64>(
                                     std::filesystem::
                                         file_size(path))}
                            });
                    }
            });

        orbit::studio_ui::WorldDocumentsUi
            worldDocumentsUi(
                studioSession,
                true);
        worldDocumentsUi.Register(ui);

        orbit::studio_ui::ProjectSettingsUi
            projectSettingsUi(
                project,
                studioSession);
        projectSettingsUi.Register(ui);

        orbit::studio_ui::CelestialAuthoringUi
            celestialAuthoringUi(
                studioSession);
        celestialAuthoringUi.Register(ui);

        orbit::studio_ui::SystemViewUi
            systemViewUi(
                studioSession);
        systemViewUi.Register(ui);

        orbit::studio_ui::SurfaceAuthoringUi
            surfaceAuthoringUi(
                studioSession);
        surfaceAuthoringUi.Register(ui);

        orbit::studio_ui::VolumeAuthoringUi
            volumeAuthoringUi(
                studioSession,
                volumeFieldStorage,
                surfaceVolumeSolver,
                studioViewportRenderer);
        volumeAuthoringUi.Register(ui);

        if (terrainUiSmoke)
        {
            surfaceAuthoringUi.
                SetAutomationCoverageMode(
                    true);
            ui.SetAutomationUiProbe(
                true,
                true);
            ui.ClearAutomationUiTrace();

            if (!ui.HasPanel(
                    orbit::studio_ui::
                        SurfaceAuthoringUi::kPanel) ||
                !ui.HasPanel(
                    orbit::studio_ui::
                        CelestialAuthoringUi::kPanel) ||
                !ui.HasPanel(
                    orbit::studio_ui::
                        SystemViewUi::kPanel) ||
                !ui.HasPanel(
                    orbit::studio_ui::
                        ProjectSettingsUi::
                            kPanelId))
            {
                throw std::runtime_error(
                    "Studio UI smoke preflight failed: active authoring/validation panels are not registered.");
            }

            static_cast<void>(
                ui.SetPanelOpen(
                    orbit::studio_ui::
                        SurfaceAuthoringUi::kPanel,
                    true));

            static_cast<void>(
                ui.SetPanelOpen(
                    orbit::studio_ui::
                        CelestialAuthoringUi::kPanel,
                    true));

            static_cast<void>(
                ui.SetPanelOpen(
                    orbit::studio_ui::
                        SystemViewUi::kPanel,
                    true));

            static_cast<void>(
                ui.SetPanelOpen(
                    orbit::studio_ui::
                        ProjectSettingsUi::
                            kPanelId,
                    true));
        }

        std::vector<orbit::editor_ui::PanelId>
            pluginPanelIds;
        orbit::u64 pluginPanelRevision =
            ~orbit::u64{0};
        orbit::f64 pluginReloadAccumulator =
            0.0;
        // Route planning and derived path products (route requests, route/derived events, the
        // path.route / path.geometry RPC queries) live in StudioPathNetworkHost.
        orbit::editor_app::StudioPathNetworkHost pathNetwork(
            worldSession,
            studioSession,
            project,
            content,
            rpcHost);
        pathNetwork.AttachRpc();

        if (worldSession.HasWorld())
        {
            pathNetwork.RequestRoutes();
        }

        orbit::u64 publishedObjectRevision =
            worldSession.HasWorld()
                ? objects().Revision()
                : 0;
        orbit::u64 publishedSelectionRevision =
            worldSession.HasWorld()
                ? selection().Revision()
                : 0;
        orbit::u64 publishedContentRevision =
            content.Revision();
        orbit::u32 publishedViewportWidth =
            primaryStudioView->Width();
        orbit::u32 publishedViewportHeight =
            primaryStudioView->Height();

        const auto publishAutomationChanges =
            [&]
            {
                if (worldSession.HasWorld() &&
                    objects().Revision() !=
                        publishedObjectRevision)
                {
                    publishedObjectRevision =
                        objects().Revision();

                    rpcHost.PublishEvent(
                        "object.changed",
                        orbit::rpc::Value(
                            orbit::rpc::Value::Object{
                                {
                                    "revision",
                                    static_cast<orbit::i64>(
                                        publishedObjectRevision)
                                }
                            }));
                }

                if (worldSession.HasWorld() &&
                    selection().Revision() !=
                        publishedSelectionRevision)
                {
                    publishedSelectionRevision =
                        selection().Revision();

                    orbit::rpc::Value::Array ids;
                    ids.reserve(
                        selection().Ordered().size());

                    for (const auto id :
                         selection().Ordered())
                    {
                        ids.emplace_back(
                            id.ToString());
                    }

                    rpcHost.PublishEvent(
                        "selection.changed",
                        orbit::rpc::Value(
                            orbit::rpc::Value::Object{
                                {
                                    "revision",
                                    static_cast<orbit::i64>(
                                        publishedSelectionRevision)
                                },
                                {
                                    "ids",
                                    std::move(ids)
                                }
                            }));
                }

                if (!worldSession.HasWorld())
                {
                    publishedObjectRevision = 0;
                    publishedSelectionRevision = 0;
                }

                if (content.Revision() !=
                    publishedContentRevision)
                {
                    publishedContentRevision =
                        content.Revision();

                    rpcHost.PublishEvent(
                        "content.changed",
                        orbit::rpc::Value(
                            orbit::rpc::Value::Object{
                                {
                                    "revision",
                                    static_cast<orbit::i64>(
                                        publishedContentRevision)
                                },
                                {
                                    "diagnostics",
                                    static_cast<orbit::i64>(
                                        content.Diagnostics().
                                            size())
                                }
                            }));
                }

                if (primaryStudioView->Width() !=
                        publishedViewportWidth ||
                    primaryStudioView->Height() !=
                        publishedViewportHeight)
                {
                    publishedViewportWidth =
                        primaryStudioView->Width();
                    publishedViewportHeight =
                        primaryStudioView->Height();

                    rpcHost.PublishEvent(
                        "viewport.resized",
                        orbit::rpc::Value(
                            orbit::rpc::Value::Object{
                                {
                                    "width",
                                    static_cast<orbit::i64>(
                                        publishedViewportWidth)
                                },
                                {
                                    "height",
                                    static_cast<orbit::i64>(
                                        publishedViewportHeight)
                                }
                            }));
                }

                for (const std::string& notification :
                     rpcHost.DrainNotifications())
                {
                    static_cast<void>(
                        rpcServer.SendServerMessage(
                            notification));
                }
            };

        orbit::editor_app::StudioInspectorTarget inspectorTarget;
        orbit::editor_app::StudioPanelEnvironment panelEnvironment{
            ui,
            worldSession,
            studioSession,
            project,
            content,
            runtime,
            window,
            outputLog,
            materialView,
            pendingMaterialViewResize,
            presentActions,
            studioViews,
            studioViewportRenderer,
            displayDiagnosticsUi,
            inspectorTarget,
            primaryTextHud,
            studioViewportPanels,
            viewportCapture,
            pendingPrimaryViewResize,
            universe,
            bodyObject,
            bodyId};

        orbit::editor_app::StudioBuildHost buildHost(panelEnvironment);
        buildHost.AttachRpc();

        orbit::editor_app::StudioViewportPanel viewportPanel(panelEnvironment);
        viewportPanel.Register();
        orbit::editor_app::StudioExplorerPanel explorerPanel(panelEnvironment);
        explorerPanel.Register();
        orbit::editor_app::StudioPropertiesPanel propertiesPanel(panelEnvironment);
        propertiesPanel.Register();
        orbit::editor_app::StudioPluginsPanel pluginsPanel(panelEnvironment);
        pluginsPanel.Register();
        orbit::editor_app::StudioMaterialServicePanel materialServicePanel(panelEnvironment);
        materialServicePanel.Register();
        orbit::editor_app::StudioPlatformServicesPanel platformServicesPanel(panelEnvironment);
        platformServicesPanel.Register();
        buildHost.Register();
        orbit::editor_app::StudioOutputPanel outputPanel(panelEnvironment);
        outputPanel.Register();
        ui.RegisterMenuAction({
            .menu = "File",
            .label = "Save Project",
            .invoke =
                [&project,
                 &world,
                 &worldSession]
                {
                    project.Save();
                    if (worldSession.HasWorld())
                    {
                        world().Checkpoint();
                    }

                    orbit::log::Info(
                        "Project and world checkpoint saved.");
                }
        });

        ui.RegisterMenuAction({
            .menu = "Build",
            .label = "Validate Project",
            .invoke =
                [&buildHost]
                {
                    buildHost.ValidateProjectBuild();
                }
        });

        ui.RegisterMenuAction({
            .menu = "Build",
            .label = "Cook Project",
            .invoke =
                [&buildHost]
                {
                    buildHost.CookProjectBuild();
                }
        });

        ui.RegisterMenuAction({
            .menu = "Build",
            .label = "Package Project",
            .invoke =
                [&buildHost]
                {
                    static_cast<void>(
                        buildHost.PackageProjectBuild(
                            buildHost.selectedBuildProfile));
                }
        });

        // Menu items for shared commands go through the same registry,
        // enablement and error handling as the toolbars and context menus, so
        // a menu can never offer something the toolbar would refuse.
        const auto registerCommandMenu =
            [&ui,
             &authoringCommands,
             &worldSession](
                std::string menu,
                std::string label,
                const orbit::commands::CommandId id,
                std::string shortcut = {})
            {
                ui.RegisterMenuAction({
                    .menu = std::move(menu),
                    .label = std::move(label),
                    .invoke =
                        [&authoringCommands,
                         &worldSession,
                         id]
                        {
                            if (!worldSession.HasWorld())
                            {
                                return;
                            }

                            try
                            {
                                authoringCommands().Invoke(id);
                            }
                            catch (
                                const std::exception&
                                    exception)
                            {
                                orbit::log::Warning(
                                    exception.what());
                            }
                        },
                    .enabled =
                        [&authoringCommands,
                         &worldSession,
                         id]
                        {
                            return worldSession.HasWorld() &&
                                authoringCommands().
                                    Enablement(id).
                                    enabled;
                        },
                    .shortcut = std::move(shortcut)
                });
            };

        // Opens a panel and brings it to the front of its tab group.
        const auto registerPanelMenu =
            [&ui](
                std::string menu,
                std::string label,
                const orbit::editor_ui::PanelId panel)
            {
                ui.RegisterMenuAction({
                    .menu = std::move(menu),
                    .label = std::move(label),
                    .invoke =
                        [&ui, panel]
                        {
                            static_cast<void>(
                                ui.FocusPanel(panel));
                        }
                });
            };

        namespace commands_ns =
            orbit::editor_model::authoring_commands;

        registerPanelMenu(
            "File",
            "Project Settings",
            orbit::studio_ui::ProjectSettingsUi::
                kPanelId);

        registerCommandMenu(
            "Home", "Undo", commands_ns::kUndo, "Ctrl+Z");
        registerCommandMenu(
            "Home", "Redo", commands_ns::kRedo, "Ctrl+Y");
        registerCommandMenu(
            "Home",
            "Clear Selection",
            commands_ns::kClearSelection);

        registerCommandMenu(
            "Model",
            "Add Terrain Surface",
            commands_ns::kCreateTerrainSurface);
        registerCommandMenu(
            "Model",
            "Remove Terrain Surface",
            commands_ns::kRemoveTerrainSurface);
        registerCommandMenu(
            "Model",
            "Move To Root",
            commands_ns::kMoveToRoot);
        registerPanelMenu(
            "Model",
            "Surface Authoring",
            orbit::studio_ui::SurfaceAuthoringUi::
                kPanel);

        registerCommandMenu(
            "World",
            "Create Celestial System",
            commands_ns::kCreateCelestialSystem);
        registerCommandMenu(
            "World",
            "Create Celestial Body",
            commands_ns::kCreateCelestialBody);
        registerCommandMenu(
            "World",
            "Create Rocky Planet",
            commands_ns::kCreateRockyPlanet);
        registerPanelMenu(
            "World",
            "World Documents",
            orbit::studio_ui::WorldDocumentsUi::
                kPanelId);

        registerCommandMenu(
            "Path",
            "Connect Direct",
            commands_ns::kConnectPathDirect);
        registerCommandMenu(
            "Path",
            "Connect Bezier",
            commands_ns::kConnectPathBezier);
        registerCommandMenu(
            "Path",
            "Connect Routed",
            commands_ns::kConnectPathRouted);

        registerPanelMenu(
            "Material",
            "Material Service",
            kContentPanel);

        registerPanelMenu(
            "Build",
            "Build Settings",
            kBuildPanel);
        registerPanelMenu(
            "Build",
            "Platform Services",
            kPlatformServicesPanel);

        registerPanelMenu(
            "Plugins",
            "Plugin Manager",
            kPluginsPanel);

        if (worldSession.HasWorld())
        {
            SynchronizePluginPanels(
                ui,
                plugins,
                pluginPanelIds);
            pluginPanelRevision =
                plugins().PanelCatalogRevision();
        }

        orbit::log::Info(
            std::format(
                "Opened project '{}' ({})",
                project.Manifest().
                    displayName,
                project.Manifest().
                    projectId.ToString()));

        auto fence =
            device.CreateFence(0);

        orbit::u64 submittedFence = 0;
        orbit::u64 nextFence = 1;

        struct InFlightFrame
        {
            std::unique_ptr<orbit::rhi::CommandAllocator> allocator;
            std::unique_ptr<orbit::rhi::CommandList> commands;
            std::unique_ptr<orbit::render_graph::RenderGraph> graph;
            orbit::u64 fenceValue{0U};
        };
        std::vector<InFlightFrame> inFlightFrames(
            swapchain.BufferCount());
        for (auto& frame : inFlightFrames)
        {
            frame.allocator = device.CreateCommandAllocator(
                orbit::rhi::QueueType::Graphics);
            frame.commands = device.CreateCommandList(*frame.allocator);
        }
        orbit::u32 nextFrameSlot = 0U;
        orbit::u32 previousRadianceUpdateCount = 64U;

        using Clock =
            std::chrono::steady_clock;

        auto previous =
            Clock::now();

        // Frame pacing. The loop is vsync-bound, so an idle editor still
        // re-renders the whole scene at the display refresh rate. Render at
        // full rate while the user interacts, automation is talking to Studio
        // or terrain is still refining. A static scene is NOT capped unless
        // Eco mode is on (1 FPS, still waking immediately on input); setting
        // ORBIT_IDLE_FPS to a positive rate opts in to an idle cap while Eco
        // is off.
        orbit::i32 idleFramesPerSecond = 0;
        if (const std::string configured =
                orbit::platform::EnvironmentVariable("ORBIT_IDLE_FPS");
            !configured.empty())
        {
            try
            {
                idleFramesPerSecond = std::clamp(
                    std::stoi(configured),
                    0,
                    1000);
            }
            catch (const std::exception&)
            {
            }
        }
        constexpr auto kIdleAfter = std::chrono::milliseconds(1500);
        auto lastActivity = Clock::now();
        auto lastIterationStart = Clock::now();
        orbit::u64 lastRpcRequestCount = 0U;

        bool terrainUiSmokeValidated = false;
        orbit::u32 terrainUiSmokeAttempts = 0U;
        orbit::u32 terrainUiSmokeRenderedFrames = 0U;
        const std::filesystem::path
            terrainUiSmokeCapture =
                terrainUiSmokeRoot /
                "terrain-ui-smoke.bmp";

        const auto synchronizeActiveBodyPreview =
            [&]
            {
                const auto& activeBody =
                    studioSession.ActiveBody().Active();

                if (activeBody.has_value())
                {
                    bodyObject =
                        activeBody->semanticObject;
                    bodyId =
                        activeBody->body;
                    materialView.Camera().frame =
                        activeBody->frame;

                }
                else
                {
                    bodyObject = {};
                    bodyId = {};
                }
            };

        while (window.PumpEvents())
        {
            {
                const auto paceNow = Clock::now();
                const orbit::u64 rpcRequests = rpcHost.RequestCount();
                const bool inputActivity = window.ConsumeInputActivity();
                const bool rpcActivity = rpcRequests != lastRpcRequestCount;
                const bool busy =
                    inputActivity ||
                    rpcActivity ||
                    studioSession.Clock().Playing() ||
                    studioViewportRenderer.HasPendingTerrainWork();
                lastRpcRequestCount = rpcRequests;

                {
                    const auto patchStats =
                        studioViewportRenderer.TerrainWorkStats();
                    studioViews.SetOrbitalPatchStats(
                        "studio.primary",
                        patchStats.patchesPending,
                        patchStats.patchesResident);
                    studioViews.SetClipmapPlanStats(
                        "studio.primary",
                        studioViewportRenderer.ClipmapPlanStats(
                            "studio.primary"));
                    studioViews.SetFlatMapStatus(
                        "studio.primary",
                        studioViewportRenderer.FlatMapStatus(
                            "studio.primary"));
                    studioViews.SetFlatMapStatus(
                        "studio.map",
                        studioViewportRenderer.FlatMapStatus(
                            "studio.map"));
                    {
                        const auto clouds =
                            studioViewportRenderer.CloudDiagnostics(
                                "studio.primary");
                        std::optional<orbit::studio_ui::StudioCloudReport> cloudReport;
                        if (clouds.has_value())
                        {
                            cloudReport = orbit::studio_ui::StudioCloudReport{
                                .layerCount = clouds->layerCount,
                                .meanCoverage = clouds->meanCoverage,
                                .meanOpticalDepth = clouds->meanOpticalDepth,
                                .timeBucket = clouds->timeBucket,
                                .fingerprint = clouds->fingerprint,
                                .gpuResident = clouds->gpuResident};
                        }
                        studioViews.SetCloudReport("studio.primary", cloudReport);
                    }
                }

                if (busy)
                {
                    lastActivity = paceNow;
                }
                else if (
                    (ecoMode.load(std::memory_order_relaxed)
                        ? 1
                        : idleFramesPerSecond) > 0 &&
                    paceNow - lastActivity > kIdleAfter)
                {
                    const orbit::i32 targetFps =
                        ecoMode.load(std::memory_order_relaxed)
                            ? 1
                            : idleFramesPerSecond;
                    const auto target =
                        std::chrono::microseconds(
                            1'000'000 / targetFps);
                    const auto elapsed =
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            paceNow - lastIterationStart);
                    if (elapsed < target)
                    {
                        window.WaitForActivity(
                            static_cast<orbit::u32>(
                                (target - elapsed).count() / 1000));
                    }
                }

                if (ecoMode.load(std::memory_order_relaxed) && busy &&
                    !rpcActivity)
                {
                    constexpr auto kEcoActiveFrame =
                        std::chrono::milliseconds(67);
                    const auto elapsed =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            paceNow - lastIterationStart);
                    if (elapsed < kEcoActiveFrame)
                    {
                        window.WaitForActivity(static_cast<orbit::u32>(
                            (kEcoActiveFrame - elapsed).count()));
                        // Input wakes may arrive before the frame deadline.
                        // Keep servicing the message queue, but defer CPU and
                        // render work until 15 FPS is due. RPC bypasses this
                        // gate so automation always gets its immediate frame.
                        if (rpcHost.RequestCount() == lastRpcRequestCount)
                        {
                            continue;
                        }
                    }
                }

                lastIterationStart = Clock::now();
            }

            // Frame timing starts after the idle-pacing wait so an intentional
            // sleep is never reported as a hitch.
            orbit::profiler::BeginFrame();
            const auto loopStarted = Clock::now();
            const orbit::u32 lightingFrameSlot = nextFrameSlot;
            auto& frame = inFlightFrames[lightingFrameSlot];
            if (frame.fenceValue != 0U)
            {
                const auto fenceWaitStarted = Clock::now();
                fence->Wait(
                    frame.fenceValue);
                cpuFrameTelemetry.Record(
                    CpuFrameTelemetry::FenceWait,
                    std::chrono::duration<double, std::milli>(
                        Clock::now() - fenceWaitStarted).count());

                if (const auto timings =
                        lightingTimestamps.ResolveCompletedFrame(
                            lightingFrameSlot);
                    timings.has_value())
                {
                    lightingScheduler.RecordGpuTimings(*timings);
                }
                surfaceVolumeSolver.ResolveGpuTimingFrame(
                    lightingFrameSlot);
                gpuPassTimer.Resolve(lightingFrameSlot);
            }
            frame.graph.reset();

            const auto frameCpuStarted =
                Clock::now();
            const auto preUiStarted = frameCpuStarted;

            const auto now =
                Clock::now();

            const orbit::f64
                deltaSeconds =
                    std::clamp(
                        std::chrono::duration<
                            orbit::f64>(
                                now -
                                previous).
                            count(),
                        1.0 / 1000.0,
                        0.1);

            previous = now;

            const orbit::u32 width =
                window.Width();
            const orbit::u32 height =
                window.Height();

            if (width == 0 ||
                height == 0)
            {
                orbit::profiler::CancelFrame();
                continue;
            }

            static_cast<void>(runtime.ResizeSwapchainToWindow());

            if (const auto wanted =
                    viewportCapture.WantedViewSize();
                wanted.has_value() &&
                (wanted->first != primaryStudioView->Width() ||
                 wanted->second != primaryStudioView->Height()))
            {
                pendingPrimaryViewResize = *wanted;
            }

            if (pendingPrimaryViewResize.has_value() ||
                pendingMaterialViewResize.has_value())
            {
                for (orbit::u32 slot = 0U;
                     slot < inFlightFrames.size();
                     ++slot)
                {
                    auto& pendingFrame = inFlightFrames[slot];
                    if (pendingFrame.fenceValue != 0U)
                    {
                        fence->Wait(pendingFrame.fenceValue);
                        if (const auto timings =
                                lightingTimestamps.ResolveCompletedFrame(slot);
                            timings.has_value())
                        {
                            lightingScheduler.RecordGpuTimings(*timings);
                        }
                        surfaceVolumeSolver.ResolveGpuTimingFrame(slot);
                    }
                    pendingFrame.graph.reset();
                }
                if (pendingPrimaryViewResize.has_value())
                {
                    primaryStudioView->Resize(
                        pendingPrimaryViewResize->first,
                        pendingPrimaryViewResize->second);
                    pendingPrimaryViewResize.reset();
                }
                if (pendingMaterialViewResize.has_value())
                {
                    materialView.Resize(
                        pendingMaterialViewResize->first,
                        pendingMaterialViewResize->second);
                    pendingMaterialViewResize.reset();
                }
                if (const auto shadingResize =
                        shadingUi.TakePreviewResizeRequest();
                    shadingResize.has_value())
                {
                    shadingView.Resize(
                        shadingResize->first,
                        shadingResize->second);
                }
            }

            rpcServer.Poll();

            studioSession.Clock().Advance(
                deltaSeconds);
            reportsController.Tick();
            viewportCapture.Tick();
            reportThumbnails.Tick();

            const auto studioTick =
                studioSession.Tick(false);

            synchronizeActiveBodyPreview();

            // The smoke run asserts a deterministic default view.
            if (!terrainUiSmoke)
            {
                viewContinuity.Tick(
                    studioViews,
                    studioSession,
                    Clock::now());
            }

            if (terrainUiSmoke &&
                !terrainUiSmokeValidated)
            {
                ++terrainUiSmokeAttempts;

                const auto smokeRuntime =
                    studioSession.
                        TerrainRuntime().
                        Capture(
                            "studio.primary");

                if (smokeRuntime.has_value())
                {
                    orbit::editor_model::
                        SurfaceAuthoringModel
                            surface(
                                objects(),
                                commandService(),
                                selection());

                    const auto selectedSurface =
                        surface.SelectedRockyBody();

                    if (!selectedSurface.has_value())
                    {
                        throw std::runtime_error(
                            "Terrain UI smoke failed: the authored rocky planet is not selected.");
                    }

                    auto relief =
                        surface.Relief(
                            selectedSurface->
                                terrain);

                    relief.detailAmplitudeMeters +=
                        1.0;

                    surface.SetRelief(
                        selectedSurface->terrain,
                        relief);

                    auto smokeBiomes =
                        surface.Biomes(
                            selectedSurface->terrain);

                    if (smokeBiomes.empty())
                    {
                        const auto smokeBiome =
                            surface.AddBiome(
                                selectedSurface->terrain,
                                "Smoke Biome");

                        static_cast<void>(
                            surface.AddSurfaceLayer(
                                smokeBiome,
                                orbit::terrain_biome::
                                    BiomeSurfaceLayerKind::
                                        Moss));

                        static_cast<void>(
                            surface.AddScatterRule(
                                smokeBiome,
                                orbit::terrain_biome::
                                    BiomeScatterKind::
                                        Tree));

                        static_cast<void>(
                            surface.PaintLocalOverride(
                                smokeBiome,
                                {0.0, 1.0, 0.0},
                                100.0,
                                1'000.0,
                                0.75,
                                1.0));
                    }

                    if (surface.TerrainConstraints(
                            selectedSurface->terrain).
                            empty())
                    {
                        static_cast<void>(
                            surface.AddHeightBrush(
                                selectedSurface->terrain,
                                {0.0, 1.0, 0.0},
                                100.0,
                                750.0,
                                25.0));
                    }

                    surface.SelectObject(
                        selectedSurface->body);

                    static_cast<void>(
                        studioSession.Tick(false));

                    studioSession.
                        Viewports().
                        SetMode(
                            "studio.map",
                            orbit::studio_session::
                                ViewportMode::Debug);

                    studioViews.SetDebugField(
                        "studio.map",
                        orbit::terrain_debug::
                            TerrainDebugField::
                                PhysicalLod);

                    studioViews.
                        SetTerrainDiagnosticOverlays(
                            "studio.primary",
                            {
                                .dirtyPageBounds = true,
                                .buildStates = true,
                                .physicalLod = true,
                                .clipmapRings = true,
                                .cacheStatus = true,
                                .authoredConstraints = true,
                                .biomeWeights = true,
                                .processMasks = true,
                                .drainageVectors = true
                            });

#if defined(ORBIT_ENABLE_VALIDATION_TOOLS)
                    const auto m15Report =
                        projectSettingsUi.
                            RunTerrainValidationScenario();

                    if (!m15Report.success)
                    {
                        throw std::runtime_error(
                            std::format(
                                "Terrain UI smoke M15 failed at {}: {}",
                                m15Report.failureStage.
                                        empty()
                                    ? std::string(
                                          "unknown")
                                    : m15Report.
                                          failureStage,
                                m15Report.diagnostic));
                    }

#endif

                    const auto roundTripReport =
                        projectSettingsUi.
                            RunTerrainRoundTripValidation();

                    if (!roundTripReport.success)
                    {
                        throw std::runtime_error(
                            std::format(
                                "Terrain UI smoke round trip failed at {}: {}",
                                roundTripReport.
                                        failureStage.
                                        empty()
                                    ? std::string(
                                          "unknown")
                                    : roundTripReport.
                                          failureStage,
                                roundTripReport.
                                    diagnostic));
                    }

                    terrainUiSmokeValidated =
                        true;

                    orbit::log::Info(
                        "Terrain UI smoke: authoring and reopen verification passed.");
                }
                else if (terrainUiSmokeAttempts >
                         240U)
                {
                    throw std::runtime_error(
                        "Terrain UI smoke failed: production terrain runtime never became available.");
                }
            }

            if (studioTick.pathRoutingRebound)
            {
                pathNetwork.OnRoutingRebound();
            }

            if (studioTick.pathProductsInvalidated)
            {
                pathNetwork.OnPathProductsInvalidated();
            }

            if (worldSession.HasWorld())
            {
                pathNetwork.PollRoutes();
                pathNetwork.RefreshDerivedPaths();
            }
            else
            {
                pathNetwork.Clear();
                viewportPanel.activePathNetwork.reset();
                viewportPanel.lastPlacedPathNode.reset();
                viewportPanel.pathPlacementMode = false;
            }

            pluginReloadAccumulator +=
                deltaSeconds;

            if (worldSession.HasWorld() &&
                pluginReloadAccumulator >=
                    0.5)
            {
                const orbit::u32 reloaded =
                    plugins().PollHotReload();

                if (reloaded != 0)
                {
                    orbit::log::Info(
                        std::format(
                            "Hot reloaded {} plugin{}.",
                            reloaded,
                            reloaded == 1
                                ? ""
                                : "s"));

                    rpcHost.PublishEvent(
                        "plugin.reloaded",
                        orbit::rpc::Value(
                            orbit::rpc::Value::Object{
                                {
                                    "count",
                                    static_cast<orbit::i64>(
                                        reloaded)
                                }
                            }));
                }

                pluginReloadAccumulator =
                    0.0;
            }

            if (worldSession.HasWorld())
            {
                if (plugins().PanelCatalogRevision() !=
                    pluginPanelRevision)
                {
                    SynchronizePluginPanels(
                        ui,
                        plugins,
                        pluginPanelIds);
                    pluginPanelRevision =
                        plugins().
                            PanelCatalogRevision();
                }
            }
            else
            {
                pluginPanelRevision =
                    ~orbit::u64{0};
            }

            publishAutomationChanges();

            // Shading: notice external shader/material saves, run the
            // debounced live compile and advance the preview clock.
            shadingWorkspace.Update(
                deltaSeconds);

            viewportPanel.viewportFrameDeltaSeconds =
                deltaSeconds;
            viewportPanel.viewportFrameMouseDelta =
                window.ConsumeMouseDelta();
            studioViewportPanels.SetGizmoRelativeMouseDelta(
                {
                    static_cast<orbit::f32>(
                        viewportPanel.viewportFrameMouseDelta.x),
                    static_cast<orbit::f32>(
                        viewportPanel.viewportFrameMouseDelta.y)
                });

            cpuFrameTelemetry.Record(
                CpuFrameTelemetry::PreUi,
                std::chrono::duration<double, std::milli>(
                    Clock::now() - preUiStarted).count());
            const auto uiStarted = Clock::now();
            ui.BeginFrame(
                window,
                deltaSeconds);

            if (ui.ConsumeSlashRequest())
            {
                studioViewportPanels.RequestCommandPaletteOpen();
            }

            {
                // Which project/world am I in? Refreshed a few times a
                // second; the catalog scan is not free and the values change
                // only on explicit user action.
                static orbit::u64 indicatorFrame = 0U;
                static std::string indicatorWorld;
                if ((indicatorFrame++ % 15U) == 0U)
                {
                    try
                    {
                        const auto active =
                            studioSession.ActiveWorld();
                        indicatorWorld =
                            !active.has_value()
                                ? std::string{}
                                : active->descriptor.displayName.empty()
                                    ? active->descriptor.relativePath.
                                          stem().string()
                                    : active->descriptor.displayName;
                    }
                    catch (const std::exception&)
                    {
                        indicatorWorld.clear();
                    }
                }

                const auto& manifest = project.Manifest();
                ui.SetProjectIndicator({
                    .name = manifest.displayName,
                    .detail =
                        indicatorWorld.empty()
                            ? std::string{}
                            : "World: " + indicatorWorld,
                    .tooltip =
                        project.ManifestPath().string() +
                        "\nClick to open Project Settings",
                    .onClick =
                        [&ui]
                        {
                            static_cast<void>(
                                ui.FocusPanel(
                                    orbit::studio_ui::
                                        ProjectSettingsUi::
                                            kPanelId));
                        }
                });
                projectBrowserUi.SetOpenProject(
                    manifest.displayName,
                    project.ManifestPath(),
                    indicatorWorld);
            }

            if (!pendingRpcNotifications.empty())
            for (auto& notification : pendingRpcNotifications)
            {
                ui.PushNotification(
                    std::move(notification));
            }
            pendingRpcNotifications.clear();

            ui.DrawStudioShell();
            studioViews.SetCompositionEnabled(
                "studio.map",
                ui.PanelVisible(
                    orbit::studio_ui::kSecondaryViewportPanel));
            cpuFrameTelemetry.Record(
                CpuFrameTelemetry::Ui,
                std::chrono::duration<double, std::milli>(
                    Clock::now() - uiStarted).count());
            const auto sceneUpdateStarted = Clock::now();

            if (viewportPanel.viewportRightGestureActive &&
                !window.MouseButtonDown(
                    orbit::platform::
                        MouseButton::Right))
            {
                if (window.RelativeMouseMode())
                {
                    window.SetRelativeMouseMode(
                        false);
                }

                viewportPanel.viewportRightGestureActive =
                    false;
                viewportPanel.viewportRightGestureDragged =
                    false;
                viewportPanel.viewportRightGestureDistance =
                    0;
            }

            synchronizeActiveBodyPreview();

            if (!worldSession.HasWorld())
            {
                pathNetwork.Clear();
                viewportPanel.activePathNetwork.reset();
                viewportPanel.lastPlacedPathNode.reset();
                viewportPanel.pathPlacementMode = false;
            }

            if (worldSession.HasWorld())
            {
                // Flying the camera (right mouse held) owns the keyboard:
                // movement keys must not double as letter shortcuts.
                shortcuts.Update(
                    window,
                    authoringCommands(),
                    ui.WantsKeyboard(),
                    viewportPanel.viewportRightGestureActive ||
                        window.MouseButtonDown(
                            orbit::platform::
                                MouseButton::Right));
            }

            publishAutomationChanges();

            if (pendingProjectSwitch.has_value())
            {
                projectBrowserWorkspace.
                    CloseProject();

                if (orbit::editor_app::
                        RelaunchStudioWithProject(
                            *pendingProjectSwitch))
                {
                    break;
                }

                pendingProjectSwitch.reset();
            }

            if (window.KeyDown(
                    orbit::platform::
                        Key::Escape) &&
                !ui.WantsKeyboard())
            {
                break;
            }

            cpuFrameTelemetry.Record(
                CpuFrameTelemetry::SceneUpdate,
                std::chrono::duration<double, std::milli>(
                    Clock::now() - sceneUpdateStarted).count());

            auto* allocator = frame.allocator.get();
            auto* commands = frame.commands.get();
            allocator->Reset();
            commands->Reset(*allocator);

            auto& backBuffer =
                swapchain.
                    CurrentBackBuffer();

            lightingTimestamps.BeginFrame(
                *commands,
                lightingFrameSlot);
            gpuPassTimer.BeginFrame(
                *commands,
                lightingFrameSlot);

            surfaceVolumeSolver.
                BeginGpuTimingFrame(
                    *commands,
                    lightingFrameSlot);

            // Each frame slot retains its graph and command storage until its
            // timeline fence completes on slot reuse.
            const auto renderGraphStarted = Clock::now();
            frame.graph =
                std::make_unique<
                    orbit::render_graph::RenderGraph>(
                        device);
            orbit::render_graph::RenderGraph& graph =
                *frame.graph;

            const auto materialViewTargets =
                materialView.Import(
                    graph,
                    "StudioMaterialPreview");

            const auto backBufferTarget =
                graph.ImportTexture(
                    "StudioSwapchain",
                    backBuffer,
                    orbit::rhi::
                        ResourceState::
                            Present);

            graph.AddPass(
                "Studio.MaterialPreview",
                {
                    {
                        .texture =
                            materialViewTargets.color,
                        .state =
                            orbit::rhi::ResourceState::RenderTarget,
                        .access =
                            orbit::render_graph::Access::Write
                    }
                },
                [&](orbit::rhi::CommandList& commandList,
                    const orbit::render_graph::Resources&)
                {
                    bodyPreview.Draw(
                        commandList,
                        materialView.Color(),
                        materialView.Width(),
                        materialView.Height(),
                        orbit::universe::BodyShape{
                            orbit::universe::SphereShape{
                                .radiusMeters = 1.0
                            }
                        },
                        materialView.Camera(),
                        materialServicePanel.materialPreviewMaterial);
                });

            // Shading preview. Rendered while its tab is visible or a shader
            // is open (so agents can screenshot it), skipped otherwise.
            std::optional<orbit::render_view::ImportedTargets>
                shadingTargets;

            if (ui.PanelVisible(
                    orbit::studio_ui::ShadingUi::kPanelId) ||
                !shadingWorkspace.Status().shader.empty())
            {
                shadingTargets =
                    shadingView.Import(
                        graph,
                        "StudioShading");

                // Swap in a newly compiled program (or fall back to the error
                // shader) at this frame boundary; a failure keeps the working
                // pipeline and is reported back to the tab.
                // Held for the statement: the renderer only reads the mesh
                // while uploading it.
                const auto shadingMesh =
                    shadingWorkspace.PreviewMeshData();

                const auto shadingUpdate =
                    shadingRenderer.Update(
                        shadingWorkspace.Program(),
                        shadingWorkspace.Status().
                            programRevision,
                        shadingMesh.get(),
                        shadingWorkspace.PreviewMeshStatus().
                            revision,
                        shadingWorkspace.PackedTextures(),
                        shadingWorkspace.TextureRevisions());

                if (!shadingUpdate.meshError.empty())
                {
                    orbit::log::Warning(
                        "Shading preview could not upload the mesh: " +
                        shadingUpdate.meshError);
                }

                for (const auto& textureError : shadingUpdate.textureErrors)
                {
                    if (!textureError.empty())
                    {
                        orbit::log::Warning(
                            "Shading preview could not upload a texture2d "
                            "parameter: " + textureError);
                    }
                }

                if (!shadingUpdate.error.empty())
                {
                    shadingWorkspace.ReportPipelineFailure(
                        shadingWorkspace.Status().
                            programRevision,
                        shadingUpdate.error);
                }

                const auto shadingParameters =
                    shadingWorkspace.PackedParameters();

                graph.AddPass(
                    "Studio.Shading",
                    {
                        {
                            .texture =
                                shadingTargets->color,
                            .state =
                                orbit::rhi::ResourceState::RenderTarget,
                            .access =
                                orbit::render_graph::Access::Write
                        },
                        {
                            .texture =
                                shadingTargets->depth,
                            .state =
                                orbit::rhi::ResourceState::DepthWrite,
                            .access =
                                orbit::render_graph::Access::Write
                        }
                    },
                    [&, shadingParameters](
                        orbit::rhi::CommandList& commandList,
                        const orbit::render_graph::Resources&)
                    {
                        shadingRenderer.Draw(
                            commandList,
                            shadingView.Color(),
                            shadingView.Depth(),
                            shadingView.Width(),
                            shadingView.Height(),
                            shadingWorkspace.Preview(),
                            shadingParameters,
                            shadingWorkspace.PreviewTime());
                        shadingRenderedOnce = true;
                    });
            }

            const auto studioSnapshot =
                studioRuntime.Capture();

            constexpr double kRadianceCpuBudgetMs = 4.0;
            const double previousGiEstimateMs =
                cpuFrameTelemetry.RollingAverageMs(
                    CpuFrameTelemetry::GiEstimate);
            auto lightingPlan =
                lightingScheduler.BuildPlan(
                    {
                        .exactVisibilityQueries = 2'048U,
                        .radianceCacheUpdates = 64U,
                        .reflectionQueries = 2'048U
                    },
                    device.Capabilities().rayQuery);
            if (previousRadianceUpdateCount > 0U &&
                previousGiEstimateMs > kRadianceCpuBudgetMs)
            {
                const auto viewCount = std::max<std::size_t>(
                    studioViews.Catalog().size(),
                    1U);
                const auto totalCpuBudgetedUpdates =
                    static_cast<orbit::u32>(
                        kRadianceCpuBudgetMs *
                        static_cast<double>(previousRadianceUpdateCount) /
                        previousGiEstimateMs);
                const orbit::u32 perViewCpuCap =
                    std::clamp<orbit::u32>(
                        static_cast<orbit::u32>(
                            totalCpuBudgetedUpdates / viewCount),
                        1U,
                        64U);
                lightingPlan.radianceCacheUpdates =
                    std::min(
                        lightingPlan.radianceCacheUpdates,
                        perViewCpuCap);
            }

            const auto viewportComposeStarted = Clock::now();
            std::array<double, 12U> composeStageTotals{};
            std::array<double, 2U> giSnapshotStageTotals{};
            orbit::u32 radianceUpdateCount = 0U;
            const auto renderedStudioViews =
                studioViewportRenderer.Compose(
                    graph,
                    studioViews,
                    studioSession,
                    studioRuntime,
                    studioSnapshot,
                    studioSession.Clock().Time(),
                    viewportPanel.pathDebugVisualization,
                    lightingFrameSlot,
                    lightingPlan,
                    &lightingTimestamps,
                    [&composeStageTotals,
                     &giSnapshotStageTotals,
                     &radianceUpdateCount](
                        const std::string_view stage,
                        const double milliseconds)
                    {
                        if (stage == "early")
                        {
                            composeStageTotals[0] += milliseconds;
                        }
                        else if (stage == "celestial")
                        {
                            composeStageTotals[1] += milliseconds;
                        }
                        else if (stage == "terrain")
                        {
                            composeStageTotals[2] += milliseconds;
                        }
                        else if (stage == "render")
                        {
                            composeStageTotals[3] += milliseconds;
                        }
                        else if (stage == "gi")
                        {
                            composeStageTotals[4] += milliseconds;
                        }
                        else if (stage == "atmosphere")
                        {
                            composeStageTotals[5] += milliseconds;
                        }
                        else if (stage == "post")
                        {
                            composeStageTotals[6] += milliseconds;
                        }
                        else if (stage == "gi_prepare")
                        {
                            composeStageTotals[7] += milliseconds;
                        }
                        else if (stage == "gi_update_list")
                        {
                            composeStageTotals[8] += milliseconds;
                        }
                        else if (stage == "gi_estimate")
                        {
                            composeStageTotals[9] += milliseconds;
                        }
                        else if (stage == "gi_snapshot")
                        {
                            composeStageTotals[10] += milliseconds;
                        }
                        else if (stage == "gi_passes")
                        {
                            composeStageTotals[11] += milliseconds;
                        }
                        else if (stage == "gi_snapshot_build")
                        {
                            giSnapshotStageTotals[0] += milliseconds;
                        }
                        else if (stage == "gi_snapshot_upload")
                        {
                            giSnapshotStageTotals[1] += milliseconds;
                        }
                        else if (stage == "gi_update_count")
                        {
                            radianceUpdateCount +=
                                static_cast<orbit::u32>(milliseconds);
                        }
                    });
            previousRadianceUpdateCount = radianceUpdateCount;
            cpuFrameTelemetry.RecordRadianceUpdates(
                radianceUpdateCount);
            cpuFrameTelemetry.Record(
                CpuFrameTelemetry::ComposeEarly,
                composeStageTotals[0]);
            cpuFrameTelemetry.Record(
                CpuFrameTelemetry::ComposeCelestial,
                composeStageTotals[1]);
            cpuFrameTelemetry.Record(
                CpuFrameTelemetry::ComposeTerrain,
                composeStageTotals[2]);
            cpuFrameTelemetry.Record(
                CpuFrameTelemetry::ComposeRender,
                composeStageTotals[3]);
            cpuFrameTelemetry.Record(
                CpuFrameTelemetry::ComposeGi,
                composeStageTotals[7] +
                    composeStageTotals[8] +
                    composeStageTotals[9] +
                    composeStageTotals[10] +
                    composeStageTotals[11]);
            cpuFrameTelemetry.Record(
                CpuFrameTelemetry::ComposeAtmosphere,
                composeStageTotals[5]);
            cpuFrameTelemetry.Record(
                CpuFrameTelemetry::ComposePost,
                composeStageTotals[6]);
            cpuFrameTelemetry.Record(
                CpuFrameTelemetry::GiPrepare,
                composeStageTotals[7]);
            cpuFrameTelemetry.Record(
                CpuFrameTelemetry::GiUpdateList,
                composeStageTotals[8]);
            cpuFrameTelemetry.Record(
                CpuFrameTelemetry::GiEstimate,
                composeStageTotals[9]);
            cpuFrameTelemetry.Record(
                CpuFrameTelemetry::GiSnapshot,
                composeStageTotals[10]);
            cpuFrameTelemetry.Record(
                CpuFrameTelemetry::GiSnapshotBuild,
                giSnapshotStageTotals[0]);
            cpuFrameTelemetry.Record(
                CpuFrameTelemetry::GiSnapshotUpload,
                giSnapshotStageTotals[1]);
            cpuFrameTelemetry.Record(
                CpuFrameTelemetry::GiPasses,
                composeStageTotals[11]);
            cpuFrameTelemetry.Record(
                CpuFrameTelemetry::ViewportCompose,
                std::chrono::duration<double, std::milli>(
                    Clock::now() - viewportComposeStarted).count());

            graph.AddPass(
                "Studio.Canvas",
                {
                    {
                        .texture =
                            backBufferTarget,
                        .state =
                            orbit::rhi::
                                ResourceState::
                                    RenderTarget,
                        .access =
                            orbit::render_graph::
                                Access::Write
                    }
                },
                [&](orbit::rhi::CommandList&
                        commandList,
                    const orbit::render_graph::
                        Resources&)
                {
                    commandList.
                        ClearColorTarget(
                            backBuffer,
                            {
                                .red =
                                    0.018F,
                                .green =
                                    0.021F,
                                .blue =
                                    0.027F,
                                .alpha =
                                    1.0F
                            });

                    commandList.
                        SetRenderTarget(
                            backBuffer);
                });

            std::vector<
                orbit::render_graph::TextureUse>
                studioUiTextures{
                    {
                        .texture =
                            materialViewTargets.color,
                        .state =
                            orbit::rhi::
                                ResourceState::
                                    ShaderResource,
                        .access =
                            orbit::render_graph::
                                Access::Read
                    },
                    {
                        .texture =
                            backBufferTarget,
                        .state =
                            orbit::rhi::
                                ResourceState::
                                    RenderTarget,
                        .access =
                            orbit::render_graph::
                                Access::Write
                    }
                };

            for (const auto& renderedView :
                 renderedStudioViews)
            {
                studioUiTextures.push_back({
                    .texture =
                        renderedView.targets.display,
                    .state =
                        orbit::rhi::
                            ResourceState::
                                ShaderResource,
                    .access =
                        orbit::render_graph::
                            Access::Read
                });
            }

            if (shadingTargets.has_value())
            {
                studioUiTextures.push_back({
                    .texture =
                        shadingTargets->color,
                    .state =
                        orbit::rhi::
                            ResourceState::
                                ShaderResource,
                    .access =
                        orbit::render_graph::
                            Access::Read
                });
            }

            graph.AddPass(
                "Studio.Ui",
                std::move(
                    studioUiTextures),
                [&](orbit::rhi::CommandList&
                        commandList,
                    const orbit::render_graph::
                        Resources&)
                {
                    ui.Render(
                        commandList,
                        backBuffer,
                        swapchain.Width(),
                        swapchain.Height());
                });

            graph.AddPass(
                "Studio.Present",
                {
                    {
                        .texture =
                            backBufferTarget,
                        .state =
                            orbit::rhi::
                                ResourceState::
                                    Present,
                        .access =
                            orbit::render_graph::
                                Access::Read
                    }
                },
                {});

            const auto renderGraphExecuteStarted = Clock::now();
            graph.Execute(*commands, &gpuPassTimer);

            commands->Close();
            cpuFrameTelemetry.Record(
                CpuFrameTelemetry::RenderGraphExecute,
                std::chrono::duration<double, std::milli>(
                    Clock::now() - renderGraphExecuteStarted).count());
            cpuFrameTelemetry.Record(
                CpuFrameTelemetry::RenderGraphSetup,
                std::chrono::duration<double, std::milli>(
                    renderGraphExecuteStarted - renderGraphStarted).count());

            studioSession.
                TerrainPerformance().
                RecordFrameCpuSeconds(
                    std::chrono::duration<
                        orbit::f64>(
                            Clock::now() -
                            frameCpuStarted).
                        count(),
                    studioSession,
                    "studio.primary");

            const auto submitPresentStarted = Clock::now();
            graphicsQueue.Submit(
                *commands);

            swapchain.Present(true);

            submittedFence =
                nextFence++;

            graphicsQueue.Signal(
                *fence,
                submittedFence);
            frame.fenceValue = submittedFence;
            cpuFrameTelemetry.Record(
                CpuFrameTelemetry::SubmitPresent,
                std::chrono::duration<double, std::milli>(
                    Clock::now() - submitPresentStarted).count());
            cpuFrameTelemetry.Record(
                CpuFrameTelemetry::WholeLoop,
                std::chrono::duration<double, std::milli>(
                    Clock::now() - loopStarted).count());
            cpuFrameTelemetry.FinishFrame();
            orbit::profiler::EndFrame();

            nextFrameSlot =
                (nextFrameSlot + 1U) %
                static_cast<orbit::u32>(inFlightFrames.size());

            if (terrainUiSmoke &&
                terrainUiSmokeValidated)
            {
                ++terrainUiSmokeRenderedFrames;

                if (terrainUiSmokeRenderedFrames >=
                    2U)
                {
                    fence->Wait(
                        submittedFence);

                    static constexpr std::array<
                        std::string_view,
                        11> requiredTerrainWidgets{{
                            "Geology##m28-geology",
                            "Macro Amplitude (m)##m28-macro-amplitude",
                            "Terrain Processes##m28-processes",
                            "Hydraulic Iterations##m11-hydraulic-iterations",
                            "Authored Terrain##m09-authored-terrain",
                            "Biomes##m28-biomes",
                            "Temperature Min##m28-Temperature-min",
                            "Center Unit Direction##m28-mask-direction",
                            "Surface Cache / Debug##m28-cache",
                            "Save, Reopen & Verify Terrain",
                            "Run M15 Terrain Validation Scenario"
                        }};

                    for (const auto widget :
                         requiredTerrainWidgets)
                    {
                        if (!ui.
                                AutomationUiTraceContains(
                                    widget))
                        {
                            throw std::runtime_error(
                                std::format(
                                    "Terrain UI smoke failed: live ImGui control '{}' was not rendered.",
                                    widget));
                        }
                    }

                    // The default dock layout must give the production
                    // viewport most of the window without any panel
                    // covering it. A stacked layout once left it a 76px strip.
                    {
                        const auto workArea =
                            ui.AutomationWorkArea();
                        const auto viewportLayout =
                            ui.AutomationPanelLayout(
                                kViewportPanel);

                        if (!viewportLayout.found ||
                            !viewportLayout.docked ||
                            viewportLayout.width *
                                    viewportLayout.height <
                                0.30F *
                                    workArea.width *
                                    workArea.height)
                        {
                            throw std::runtime_error(
                                std::format(
                                    "Terrain UI smoke failed: viewport panel is not docked with a usable size (found={}, docked={}, {}x{} of {}x{}).",
                                    viewportLayout.found,
                                    viewportLayout.docked,
                                    viewportLayout.width,
                                    viewportLayout.height,
                                    workArea.width,
                                    workArea.height));
                        }

                        for (const auto& [name, other] :
                             {std::pair<std::string_view,
                                        orbit::editor_ui::PanelId>{
                                  "Explorer",
                                  kExplorerPanel},
                              std::pair<std::string_view,
                                        orbit::editor_ui::PanelId>{
                                  "Output",
                                  kOutputPanel}})
                        {
                            const auto probe =
                                ui.AutomationPanelLayout(
                                    other);

                            if (!probe.found)
                            {
                                continue;
                            }

                            const bool overlaps =
                                probe.x <
                                    viewportLayout.x +
                                        viewportLayout.width -
                                        1.0F &&
                                viewportLayout.x <
                                    probe.x +
                                        probe.width -
                                        1.0F &&
                                probe.y <
                                    viewportLayout.y +
                                        viewportLayout.height -
                                        1.0F &&
                                viewportLayout.y <
                                    probe.y +
                                        probe.height -
                                        1.0F;

                            if (!probe.docked ||
                                overlaps)
                            {
                                throw std::runtime_error(
                                    std::format(
                                        "Terrain UI smoke failed: '{}' panel is floating or overlaps the viewport.",
                                        name));
                            }
                        }
                    }

                    if (ui.AutomationUiTraceSize() <
                        requiredTerrainWidgets.size())
                    {
                        throw std::runtime_error(
                            "Terrain UI smoke failed: live widget trace is incomplete.");
                    }

                    const auto capture =
                        orbit::render_view::
                            CaptureBmp(
                                device,
                                graphicsQueue,
                                *primaryStudioView,
                                terrainUiSmokeCapture);

                    if (capture.fileBytes == 0U ||
                        capture.width == 0U ||
                        capture.height == 0U)
                    {
                        throw std::runtime_error(
                            "Terrain UI smoke failed: real Vulkan viewport capture is empty.");
                    }

                    orbit::log::Info(
                        std::format(
                            "Terrain UI smoke passed: {}x{} production viewport captured to {}.",
                            capture.width,
                            capture.height,
                            capture.path.
                                generic_string()));

                    break;
                }
            }
        }

        if (submittedFence != 0)
        {
            fence->Wait(
                submittedFence);
        }

        if (worldSession.HasWorld())
        {
            world().Checkpoint();
        }
        return 0;
    }
    catch (const std::exception& exception)
    {
        orbit::log::Error(
            exception.what());
        return 1;
    }
}
