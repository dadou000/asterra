#include <orbit/commands/CommandRegistry.hpp>
#include <orbit/commands/CommandService.hpp>
#include <orbit/content/ContentService.hpp>
#include <orbit/core/Log.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/dev_server/DevServer.hpp>
#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/BuiltinSchemas.hpp>
#include <orbit/editor_model/CommandSurfaces.hpp>
#include <orbit/editor_model/ExplorerModel.hpp>
#include <orbit/editor_model/InspectorModel.hpp>
#include <orbit/editor_model/OutputLog.hpp>
#include <orbit/editor_model/ShortcutRegistry.hpp>
#include <orbit/editor_rpc/EditorRpcService.hpp>
#include <orbit/editor_ui/BodyPreviewRenderer.hpp>
#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/frames/FrameGraph.hpp>
#include <orbit/platform/FileDialog.hpp>
#include <orbit/platform/Paths.hpp>
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
#include <orbit/universe/BodyRegistry.hpp>
#include <orbit/universe/ReferenceSurface.hpp>

#include <algorithm>
#include <array>
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
#include <variant>
#include <vector>

namespace
{
template <typename TargetId, typename SourceId>
[[nodiscard]] TargetId DerivedPersistentId(
    const SourceId source,
    const orbit::u64 highSalt,
    const orbit::u64 lowSalt) noexcept
{
    TargetId result{
        .high = source.high ^ highSalt,
        .low = source.low ^ lowSalt
    };

    if (!result)
    {
        result.low = 1;
    }

    return result;
}

[[nodiscard]] orbit::documents::ProjectDocument
OpenProject(
    const int argc,
    char** argv)
{
    std::filesystem::path manifest;

    if (argc >= 2 &&
        argv[1] != nullptr)
    {
        manifest =
            std::filesystem::path(
                argv[1]);

        if (std::filesystem::is_directory(
                manifest))
        {
            manifest /=
                "Project.orbit.toml";
        }
    }
    else
    {
        const std::filesystem::path local =
            std::filesystem::current_path() /
            "Project.orbit.toml";

        if (std::filesystem::exists(local))
        {
            manifest = local;
        }
    }

    if (!manifest.empty())
    {
        return orbit::documents::
            ProjectDocument::Open(
                manifest);
    }

    const std::filesystem::path scratch =
        orbit::platform::
            UserDataDirectory() /
        "Scratch" /
        "StudioPreview";

    const std::filesystem::path
        scratchManifest =
            scratch /
            "Project.orbit.toml";

    if (std::filesystem::exists(
            scratchManifest))
    {
        return orbit::documents::
            ProjectDocument::Open(
                scratchManifest);
    }

    return orbit::documents::
        ProjectDocument::Create(
            scratch,
            "Orbit Studio Preview");
}

[[nodiscard]] const char* LogPrefix(
    const orbit::log::Level level) noexcept
{
    switch (level)
    {
    case orbit::log::Level::Trace:
        return "TRACE";
    case orbit::log::Level::Info:
        return "INFO";
    case orbit::log::Level::Warning:
        return "WARN";
    case orbit::log::Level::Error:
        return "ERROR";
    }

    return "LOG";
}

[[nodiscard]] std::optional<
    orbit::scene::ObjectId>
FindFirstBodyObject(
    orbit::scene::ObjectStore& objects)
{
    std::vector<orbit::scene::ObjectRecord>
        pending =
            objects.Roots();

    while (!pending.empty())
    {
        const orbit::scene::ObjectRecord
            object =
                pending.back();
        pending.pop_back();

        if (object.type ==
            orbit::editor_model::builtin::
                kCelestialBodyType)
        {
            return object.id;
        }

        auto children =
            objects.Children(
                object.id);

        pending.insert(
            pending.end(),
            children.begin(),
            children.end());
    }

    return std::nullopt;
}

[[nodiscard]] orbit::scene::ObjectId
EnsureInitialBodyObject(
    orbit::scene::ObjectStore& objects,
    orbit::commands::CommandService& commands)
{
    if (const auto existing =
            FindFirstBodyObject(objects);
        existing.has_value())
    {
        return *existing;
    }

    commands.BeginTransaction(
        "Initialize World");

    try
    {
        const auto roots =
            objects.Roots();

        orbit::scene::ObjectId worldRoot{};

        if (roots.empty())
        {
            worldRoot =
                commands.CreateObject(
                    orbit::editor_model::
                        builtin::kWorldType,
                    "World");
        }
        else
        {
            worldRoot =
                roots.front().id;
        }

        const orbit::scene::ObjectId body =
            commands.CreateObject(
                orbit::editor_model::
                    builtin::
                        kCelestialBodyType,
                "Asterra",
                worldRoot);

        commands.SetProperty(
            body,
            orbit::editor_model::
                builtin::kBodyRadius,
            6'000'000.0);

        commands.SetProperty(
            body,
            orbit::editor_model::
                builtin::kBodyMass,
            5.0e24);

        commands.CommitTransaction();
        return body;
    }
    catch (...)
    {
        commands.RollbackTransaction();
        throw;
    }
}

[[nodiscard]] std::array<
    std::byte,
    sizeof(orbit::scene::ObjectId)>
EncodeObjectId(
    const orbit::scene::ObjectId id)
{
    static_assert(
        std::is_trivially_copyable_v<
            orbit::scene::ObjectId>);

    std::array<
        std::byte,
        sizeof(orbit::scene::ObjectId)>
        bytes{};

    std::memcpy(
        bytes.data(),
        &id,
        sizeof(id));

    return bytes;
}

[[nodiscard]] std::optional<
    orbit::scene::ObjectId>
DecodeObjectId(
    const std::vector<std::byte>& bytes)
{
    if (bytes.size() !=
        sizeof(orbit::scene::ObjectId))
    {
        return std::nullopt;
    }

    orbit::scene::ObjectId id{};

    std::memcpy(
        &id,
        bytes.data(),
        sizeof(id));

    return id.IsValid()
        ? std::optional(id)
        : std::nullopt;
}

[[nodiscard]] std::array<
    std::byte,
    sizeof(orbit::content::AssetId)>
EncodeAssetId(
    const orbit::content::AssetId id)
{
    static_assert(
        std::is_trivially_copyable_v<
            orbit::content::AssetId>);

    std::array<
        std::byte,
        sizeof(orbit::content::AssetId)>
        bytes{};

    std::memcpy(
        bytes.data(),
        &id,
        sizeof(id));

    return bytes;
}

[[nodiscard]] std::optional<
    orbit::content::AssetId>
DecodeAssetId(
    const std::vector<std::byte>& bytes)
{
    if (bytes.size() !=
        sizeof(orbit::content::AssetId))
    {
        return std::nullopt;
    }

    orbit::content::AssetId id{};

    std::memcpy(
        &id,
        bytes.data(),
        sizeof(id));

    return id.IsValid()
        ? std::optional(id)
        : std::nullopt;
}

void SynchronizePluginPanels(
    orbit::editor_ui::EditorUi& ui,
    orbit::plugins::PluginManager& plugins,
    std::vector<orbit::editor_ui::PanelId>&
        registered)
{
    const auto catalog =
        plugins.PanelCatalog();

    for (auto item = registered.begin();
         item != registered.end();)
    {
        const bool stillPresent =
            std::find_if(
                catalog.begin(),
                catalog.end(),
                [id = *item](const auto& panel)
                {
                    return panel.id == id;
                }) != catalog.end();

        if (stillPresent)
        {
            ++item;
            continue;
        }

        static_cast<void>(
            ui.UnregisterPanel(*item));
        item = registered.erase(item);
    }

    for (const auto& panel : catalog)
    {
        const auto id = panel.id;

        ui.UpsertPanel({
            .id = id,
            .title = panel.title,
            .defaultOpen = false,
            .draw =
                [&plugins, id](
                    orbit::editor_ui::PanelContext&
                        context)
                {
                    if (!plugins.DrawPanel(
                            id,
                            context))
                    {
                        context.Text(
                            "Plugin panel is unavailable.");
                    }
                }
        });

        if (std::find(
                registered.begin(),
                registered.end(),
                id) == registered.end())
        {
            registered.push_back(id);
        }
    }
}

[[nodiscard]] orbit::f64 BodyRadius(
    orbit::scene::ObjectStore& objects,
    const orbit::scene::ObjectId body)
{
    const auto value =
        objects.GetProperty(
            body,
            orbit::editor_model::
                builtin::kBodyRadius);

    if (value.has_value())
    {
        if (const auto* radius =
                std::get_if<orbit::f64>(
                    &*value))
        {
            return std::max(
                *radius,
                1.0);
        }
    }

    return 6'000'000.0;
}
} // namespace

int main(
    const int argc,
    char** argv)
{
    try
    {
        orbit::editor_model::OutputLog
            outputLog;

        orbit::documents::ProjectDocument
            project =
                OpenProject(
                    argc,
                    argv);

        orbit::content::ContentService
            content(
                project.RootDirectory());

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

        orbit::documents::WorldDatabase
            world(
                project.StartupWorldPath());

        orbit::schema::SchemaRegistry
            schemas;

        orbit::editor_model::builtin::
            RegisterSchemas(schemas);

        orbit::scene::ObjectStore objects(
            world);

        orbit::selection::SelectionService
            selection;

        orbit::commands::CommandService
            commandService(
                objects,
                schemas);

        orbit::editor_model::ExplorerModel
            explorer(
                objects,
                commandService,
                selection);

        orbit::editor_model::InspectorModel
            inspector(
                objects,
                schemas,
                commandService,
                selection);

        const orbit::scene::ObjectId
            bodyObject =
                EnsureInitialBodyObject(
                    objects,
                    commandService);

        const std::array initialSelection{
            bodyObject
        };

        selection.Set(
            std::span(
                initialSelection));

        orbit::commands::CommandRegistry
            authoringCommands;

        orbit::editor_model::
            authoring_commands::Register(
                authoringCommands,
                commandService,
                objects,
                selection);

        orbit::editor_model::
            CommandSurfaceRegistry
                commandSurfaces;

        orbit::plugins::PluginManager
            plugins(
                project.RootDirectory(),
                authoringCommands,
                commandService,
                commandSurfaces,
                objects,
                selection);

        plugins.LoadEnabled(
            project.Manifest());

        orbit::rpc::Dispatcher
            rpcDispatcher;

        orbit::editor_rpc::EditorRpcService
            editorRpc(
                rpcDispatcher,
                project,
                authoringCommands,
                commandService,
                schemas,
                objects,
                selection);

        orbit::dev_server::DevServer
            rpcServer({
                .port = 4320,
                .maxMessageBytes =
                    1024U * 1024U
            });

        rpcServer.SetMessageHandler(
            [&rpcDispatcher](
                const std::string_view message)
            {
                return rpcDispatcher.Dispatch(
                    message);
            });

        commandSurfaces.Set(
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
                        kConnectPathBezier
            });

        commandSurfaces.Set(
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
                    authoring_commands::kUndo,
                orbit::editor_model::
                    authoring_commands::kRedo
            });

        commandSurfaces.Set(
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
             &authoringCommands](
                const std::string_view surface,
                const orbit::editor_model::
                    CommandSurfaceKind kind)
            {
                std::vector<
                    orbit::editor_ui::
                        ActionPresentation>
                    result;

                for (const auto& command :
                     commandSurfaces.Present(
                         surface,
                         kind,
                         authoringCommands))
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
                                    authoringCommands.
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

        orbit::runtime::RuntimeSession runtime({
            .applicationName = "OrbitStudio",
            .windowTitle = windowTitle,
            .width = 1680,
            .height = 980,
            .swapchainBufferCount = 3,
            .allowTearing = true,
            .relativeMouseMode = false
        });

        orbit::platform::Window& window =
            runtime.Window();
        orbit::rhi::Device& device =
            runtime.Device();
        orbit::rhi::Queue& graphicsQueue =
            runtime.GraphicsQueue();
        orbit::rhi::Swapchain& swapchain =
            runtime.Swapchain();

        const orbit::shader::dxc::
            DxcShaderCompiler compiler;

        // Studio still exercises the runtime celestial registry instead
        // of owning a separate editor-only body representation. The
        // semantic ObjectRecord above is the persisted authoring record;
        // the registry is the active runtime representation.
        orbit::frames::FrameGraph frames;
        orbit::universe::BodyRegistry bodies(
            frames);

        const auto projectId =
            project.Manifest().projectId;

        const orbit::universe::SystemId
            persistentSystemId =
                DerivedPersistentId<
                    orbit::universe::SystemId>(
                        projectId,
                        0x53595354454d4944ULL,
                        0x4f52424954563033ULL);

        const orbit::frames::FrameId
            persistentSystemFrame =
                DerivedPersistentId<
                    orbit::frames::FrameId>(
                        projectId,
                        0x5359534652414d45ULL,
                        0x4f52424954563033ULL);

        const orbit::universe::SystemId
            system =
                bodies.CreateSystem(
                    "Helion",
                    persistentSystemId,
                    persistentSystemFrame);

        const orbit::universe::BodyId
            persistentBodyId =
                DerivedPersistentId<
                    orbit::universe::BodyId>(
                        bodyObject,
                        0x424f445949440003ULL,
                        0x4f52424954563033ULL);

        const orbit::frames::FrameId
            persistentBodyFrame =
                DerivedPersistentId<
                    orbit::frames::FrameId>(
                        bodyObject,
                        0x424f44594652414dULL,
                        0x4f52424954563033ULL);

        const orbit::universe::BodyId
            bodyId =
                bodies.CreateBody({
                    .system = system,
                    .name = "Asterra",
                    .shape =
                        orbit::universe::
                            SphereShape{
                                .radiusMeters =
                                    BodyRadius(
                                        objects,
                                        bodyObject)
                            },
                    .mass =
                        orbit::universe::
                            MassProperties{
                                .massKilograms =
                                    5.0e24
                            },
                    .id =
                        persistentBodyId,
                    .frame =
                        persistentBodyFrame
                });

        if (bodies.FindBody(bodyId) ==
            nullptr)
        {
            throw std::runtime_error(
                "Studio failed to create its active body.");
        }

        orbit::render_view::RenderView
            bodyView(
                device,
                {
                    .width = 960,
                    .height = 640
                });

        const orbit::f64 initialBodyRadius =
            BodyRadius(
                objects,
                bodyObject);

        bodyView.Camera().frame =
            bodies.FindBody(bodyId)->frame;
        bodyView.Camera().
            localPositionMeters = {
                0.0,
                0.0,
                -initialBodyRadius * 3.2
            };
        bodyView.Camera().nearPlaneMeters =
            static_cast<orbit::f32>(
                std::max(
                    initialBodyRadius *
                        1.0e-6,
                    1.0));
        bodyView.Camera().farPlaneMeters =
            static_cast<orbit::f32>(
                initialBodyRadius * 10.0);
        bodyView.Camera().forward = {
            0.0F,
            0.0F,
            1.0F
        };
        bodyView.Camera().up = {
            0.0F,
            1.0F,
            0.0F
        };

        editorRpc.AttachViewport({
            .view = &bodyView,
            .capture =
                [&device,
                 &graphicsQueue,
                 &bodyView](
                    const std::filesystem::path&
                        path)
                {
                    return orbit::render_view::
                        CaptureBmp(
                            device,
                            graphicsQueue,
                            bodyView,
                            path);
                }
        });

        orbit::editor_ui::
            BodyPreviewRenderer
                bodyPreview(
                    device,
                    compiler);

        const std::filesystem::path
            layoutPath =
                orbit::platform::
                    UserDataDirectory() /
                "EditorLayouts" /
                (project.Manifest().
                     projectId.ToString() +
                 ".ini");

        orbit::editor_ui::EditorUi ui(
            device,
            graphicsQueue,
            compiler,
            layoutPath);

        constexpr orbit::editor_ui::PanelId
            kViewportPanel{
                .high =
                    0x4f52424954535455ULL,
                .low =
                    0x44494f5657455750ULL
            };

        constexpr orbit::editor_ui::PanelId
            kExplorerPanel{
                .high =
                    0x4f52424954535455ULL,
                .low =
                    0x44494f4558504c52ULL
            };

        constexpr orbit::editor_ui::PanelId
            kPropertiesPanel{
                .high =
                    0x4f52424954535455ULL,
                .low =
                    0x44494f50524f5053ULL
            };

        constexpr orbit::editor_ui::PanelId
            kOutputPanel{
                .high =
                    0x4f52424954535455ULL,
                .low =
                    0x44494f4f55545054ULL
            };

        constexpr orbit::editor_ui::PanelId
            kContentPanel{
                .high =
                    0x4f52424954535455ULL,
                .low =
                    0x44494f434f4e544eULL
            };

        constexpr orbit::editor_ui::PanelId
            kPluginsPanel{
                .high =
                    0x4f52424954535455ULL,
                .low =
                    0x44494f504c554749ULL
            };

        std::string explorerSearch;
        std::string contentSearch;
        std::string renameBuffer;
        std::vector<orbit::editor_ui::PanelId>
            pluginPanelIds;
        std::vector<
            orbit::plugins::PluginValidationIssue>
            pluginValidationIssues;
        orbit::u64 pluginPanelRevision =
            ~orbit::u64{0};
        orbit::f64 pluginReloadAccumulator =
            0.0;
        orbit::u64 renameSelectionRevision =
            ~orbit::u64{0};

        bool pathPlacementMode = false;
        std::optional<orbit::paths::NetworkId>
            activePathNetwork;
        std::optional<orbit::scene::ObjectId>
            lastPlacedPathNode;

        orbit::u64 publishedObjectRevision =
            objects.Revision();
        orbit::u64 publishedSelectionRevision =
            selection.Revision();
        orbit::u64 publishedContentRevision =
            content.Revision();
        orbit::u32 publishedViewportWidth =
            bodyView.Width();
        orbit::u32 publishedViewportHeight =
            bodyView.Height();

        const auto publishAutomationChanges =
            [&]
            {
                if (objects.Revision() !=
                    publishedObjectRevision)
                {
                    publishedObjectRevision =
                        objects.Revision();

                    editorRpc.PublishEvent(
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

                if (selection.Revision() !=
                    publishedSelectionRevision)
                {
                    publishedSelectionRevision =
                        selection.Revision();

                    orbit::rpc::Value::Array ids;
                    ids.reserve(
                        selection.Ordered().size());

                    for (const auto id :
                         selection.Ordered())
                    {
                        ids.emplace_back(
                            id.ToString());
                    }

                    editorRpc.PublishEvent(
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

                if (content.Revision() !=
                    publishedContentRevision)
                {
                    publishedContentRevision =
                        content.Revision();

                    editorRpc.PublishEvent(
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

                if (bodyView.Width() !=
                        publishedViewportWidth ||
                    bodyView.Height() !=
                        publishedViewportHeight)
                {
                    publishedViewportWidth =
                        bodyView.Width();
                    publishedViewportHeight =
                        bodyView.Height();

                    editorRpc.PublishEvent(
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
                     editorRpc.DrainNotifications())
                {
                    static_cast<void>(
                        rpcServer.SendServerMessage(
                            notification));
                }
            };

        ui.RegisterPanel({
            .id = kViewportPanel,
            .title = "Viewport",
            .defaultOpen = true,
            .draw =
                [&bodyView,
                 &selection,
                 bodyObject,
                 &objects,
                 &content,
                 &authoringCommands,
                 &presentActions,
                 &commandService,
                 &pathPlacementMode,
                 &activePathNetwork,
                 &lastPlacedPathNode,
                 &bodies,
                 bodyId](
                    orbit::editor_ui::
                        PanelContext& context)
                {
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

                    if (width !=
                            bodyView.Width() ||
                        height !=
                            bodyView.Height())
                    {
                        bodyView.Resize(
                            width,
                            height);
                    }

                    const auto interaction =
                        context.Image(
                            bodyView.Color(),
                            {
                                .width =
                                    static_cast<
                                        orbit::f32>(
                                            bodyView.
                                                Width()),
                                .height =
                                    static_cast<
                                        orbit::f32>(
                                            bodyView.
                                                Height())
                            });

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

                                selection.Set(
                                    std::span(
                                        selected));

                                try
                                {
                                    authoringCommands.
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
                            else
                            {
                                orbit::log::Warning(
                                    "Viewport drop expects a material asset.");
                            }
                        }
                    }

                    if (interaction.clicked &&
                        pathPlacementMode)
                    {
                        try
                        {
                            const auto* body =
                                bodies.FindBody(
                                    bodyId);

                            const auto ray =
                                orbit::render_view::
                                    ViewportRay(
                                        bodyView.Camera(),
                                        bodyView.Width(),
                                        bodyView.Height(),
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

                            orbit::paths::
                                PathNetworkService
                                    pathService(
                                        objects,
                                        commandService);

                            std::optional<
                                orbit::paths::NetworkId>
                                targetNetwork;

                            if (selection.Ordered().
                                    size() == 1)
                            {
                                const auto selectedObject =
                                    objects.Find(
                                        selection.Ordered().
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

                            commandService.
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
                                     objects.Children(
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

                                commandService.
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

                                        selection.Set(
                                            std::span(
                                                selected));
                                    }
                                    else
                                    {
                                        const std::array
                                            selected{
                                                node.id
                                            };

                                        selection.Set(
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

                                    selection.Set(
                                        std::span(
                                            selected));
                                }

                                lastPlacedPathNode =
                                    node.id;
                            }
                            catch (...)
                            {
                                if (commandService.
                                        HasActiveTransaction())
                                {
                                    commandService.
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
                        interaction.rightClicked)
                    {
                        const std::array selected{
                            bodyObject
                        };

                        selection.Set(
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
                        interaction.rightClicked);

                    const auto body =
                        objects.Find(
                            bodyObject);

                    if (body.has_value())
                    {
                        context.Text(
                            std::format(
                                "{}{}",
                                body->name,
                                selection.Contains(
                                    bodyObject)
                                    ? "  [selected]"
                                    : ""));
                    }
                }
        });

        ui.RegisterPanel({
            .id = kExplorerPanel,
            .title = "Explorer",
            .defaultOpen = true,
            .draw =
                [&explorer,
                 &selection,
                 &explorerSearch,
                 &renameBuffer,
                 &renameSelectionRevision,
                 &objects,
                 &presentActions](
                    orbit::editor_ui::
                        PanelContext& context)
                {
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
                                explorer.Reparent(
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
                             explorer.Search(
                                 explorerSearch))
                        {
                            const std::string label =
                                object.name +
                                "##search-" +
                                object.id.ToString();

                            if (context.Selectable(
                                    label,
                                    selection.Contains(
                                        object.id)))
                            {
                                explorer.Select(
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
                                    "##tree-" +
                                    object.id.ToString();

                                const auto item =
                                    context.TreeItem(
                                        label,
                                        selection.
                                            Contains(
                                                object.id));

                                if (item.clicked)
                                {
                                    explorer.Select(
                                        object.id,
                                        context.
                                            ControlDown());
                                }

                                if (item.rightClicked &&
                                    !selection.Contains(
                                        object.id))
                                {
                                    explorer.Select(
                                        object.id,
                                        false);
                                }

                                const auto objectMenu =
                                    presentActions(
                                        "explorer",
                                        orbit::editor_model::
                                            CommandSurfaceKind::
                                                ContextMenu);

                                context.ContextMenu(
                                    "ExplorerObjectMenu##" +
                                        object.id.
                                            ToString(),
                                    objectMenu,
                                    item.rightClicked);

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
                                            explorer.
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
                                         explorer.Children(
                                             object.id))
                                    {
                                        drawObject(
                                            child);
                                    }

                                    context.TreePop();
                                }
                            };

                        for (const auto& root :
                             explorer.Roots())
                        {
                            drawObject(root);
                        }
                    }

                    context.Separator();

                    const auto& selected =
                        selection.Ordered();

                    if (selected.size() == 1)
                    {
                        if (renameSelectionRevision !=
                            selection.Revision())
                        {
                            const auto object =
                                objects.Find(
                                    selected.front());

                            renameBuffer =
                                object.has_value()
                                    ? object->name
                                    : std::string{};

                            renameSelectionRevision =
                                selection.Revision();
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
                                explorer.Rename(
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

        ui.RegisterPanel({
            .id = kPropertiesPanel,
            .title = "Properties",
            .defaultOpen = true,
            .draw =
                [&inspector,
                 &presentActions](
                    orbit::editor_ui::
                        PanelContext& context)
                {
                    const auto selected =
                        inspector.SelectedObjects();

                    if (selected.empty())
                    {
                        context.Text(
                            "No selection");
                        return;
                    }

                    context.Text(
                        std::format(
                            "{} object{} selected",
                            selected.size(),
                            selected.size() == 1
                                ? ""
                                : "s"));

                    const auto propertyActions =
                        presentActions(
                            "properties",
                            orbit::editor_model::
                                CommandSurfaceKind::
                                    Toolbar);

                    if (!propertyActions.empty())
                    {
                        context.Toolbar(
                            propertyActions);
                    }

                    context.Separator();

                    for (auto property :
                         inspector.CommonProperties())
                    {
                        context.Text(
                            std::format(
                                "{}{}{}",
                                property.schema.name,
                                property.schema.unit.empty()
                                    ? ""
                                    : " [",
                                property.schema.unit.empty()
                                    ? ""
                                    : property.schema.unit +
                                        "]"));

                        if (property.mixed)
                        {
                            context.Text(
                                "<mixed>");
                        }

                        if (property.schema.readOnly)
                        {
                            context.Text(
                                "<read only>");
                            continue;
                        }

                        const std::string label =
                            "##property-" +
                            property.schema.id.
                                ToString();

                        bool changed = false;

                        std::visit(
                            [&](auto& value)
                            {
                                using Value =
                                    std::decay_t<
                                        decltype(value)>;

                                if constexpr (
                                    std::is_same_v<
                                        Value,
                                        bool>)
                                {
                                    changed =
                                        context.
                                            Checkbox(
                                                label,
                                                value);
                                }
                                else if constexpr (
                                    std::is_same_v<
                                        Value,
                                        orbit::i64>)
                                {
                                    changed =
                                        context.
                                            InputInteger(
                                                label,
                                                value);
                                }
                                else if constexpr (
                                    std::is_same_v<
                                        Value,
                                        orbit::f64>)
                                {
                                    changed =
                                        context.
                                            InputDouble(
                                                label,
                                                value);
                                }
                                else if constexpr (
                                    std::is_same_v<
                                        Value,
                                        std::string>)
                                {
                                    changed =
                                        context.
                                            InputText(
                                                label,
                                                value);
                                }
                                else if constexpr (
                                    std::is_same_v<
                                        Value,
                                        orbit::math::
                                            Double3>)
                                {
                                    changed =
                                        context.
                                            InputDouble3(
                                                label,
                                                value);
                                }
                                else
                                {
                                    const orbit::scene::
                                        ObjectId id{
                                            .high =
                                                value.high,
                                            .low =
                                                value.low
                                        };

                                    context.Text(
                                        id.IsValid()
                                            ? id.ToString()
                                            : "<none>");
                                }
                            },
                            property.value);

                        if (property.schema.id ==
                                orbit::paths::
                                    kNetworkProfile ||
                            property.schema.id ==
                                orbit::paths::
                                    kEdgeProfileOverride)
                        {
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
                                        asset->kind ==
                                            orbit::content::
                                                AssetKind::PathProfile)
                                    {
                                        property.value =
                                            asset->id.
                                                ToString();
                                        changed = true;
                                    }
                                }
                            }
                        }

                        if (changed)
                        {
                            try
                            {
                                inspector.
                                    SetForSelection(
                                        property.schema.id,
                                        property.value);
                            }
                            catch (
                                const std::exception&
                                    exception)
                            {
                                orbit::log::Warning(
                                    exception.what());
                            }
                        }

                        context.Separator();
                    }
                }
        });

        ui.RegisterPanel({
            .id = kPluginsPanel,
            .title = "Plugins",
            .defaultOpen = false,
            .draw =
                [&plugins,
                 &pluginValidationIssues](
                    orbit::editor_ui::
                        PanelContext& context)
                {
                    const auto statuses =
                        plugins.Statuses();

                    if (context.Button(
                            "Validate"))
                    {
                        pluginValidationIssues =
                            plugins.Validate();
                    }

                    context.SameLine();

                    context.Text(
                        std::format(
                            "{} enabled plugin{}",
                            statuses.size(),
                            statuses.size() == 1
                                ? ""
                                : "s"));

                    context.Separator();

                    if (!pluginValidationIssues.
                            empty())
                    {
                        context.Text(
                            std::format(
                                "{} validation issue{}",
                                pluginValidationIssues.
                                    size(),
                                pluginValidationIssues.
                                        size() == 1
                                    ? ""
                                    : "s"));

                        for (const auto& issue :
                             pluginValidationIssues)
                        {
                            context.Text(
                                std::format(
                                    "[{}] {}",
                                    issue.pluginId,
                                    issue.message));
                        }

                        context.Separator();
                    }

                    for (const auto& status :
                         statuses)
                    {
                        context.Text(
                            std::format(
                                "{} {}  [{}]",
                                status.id,
                                status.version.empty()
                                    ? "<unknown>"
                                    : status.version,
                                status.loaded
                                    ? "loaded"
                                    : "failed"));

                        if (!status.error.empty())
                        {
                            context.Text(
                                "  " +
                                status.error);
                        }

                        const std::string reloadLabel =
                            "Reload##plugin-" +
                            status.id;

                        if (context.Button(
                                reloadLabel))
                        {
                            if (!plugins.Reload(
                                    status.id))
                            {
                                orbit::log::Warning(
                                    std::format(
                                        "Plugin '{}' reload failed.",
                                        status.id));
                            }
                        }

                        context.Separator();
                    }
                }
        });

        ui.RegisterPanel({
            .id = kContentPanel,
            .title = "Material Service",
            .defaultOpen = true,
            .draw =
                [&content,
                 &contentSearch,
                 &window](
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

                        static_cast<void>(
                            context.Selectable(
                                label,
                                false));

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

        ui.RegisterPanel({
            .id = kOutputPanel,
            .title = "Output",
            .defaultOpen = true,
            .draw =
                [&outputLog](
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

        ui.RegisterMenuAction({
            .menu = "File",
            .label = "Save Project",
            .invoke =
                [&project,
                 &world]
                {
                    project.Save();
                    world.Checkpoint();

                    orbit::log::Info(
                        "Project and world checkpoint saved.");
                }
        });

        ui.RegisterMenuAction({
            .menu = "Home",
            .label = "Undo",
            .invoke =
                [&authoringCommands]
                {
                    authoringCommands.Invoke(
                        orbit::editor_model::
                            authoring_commands::
                                kUndo);
                },
            .enabled =
                [&authoringCommands]
                {
                    return authoringCommands.
                        Enablement(
                            orbit::editor_model::
                                authoring_commands::
                                    kUndo).
                        enabled;
                }
        });

        ui.RegisterMenuAction({
            .menu = "Home",
            .label = "Redo",
            .invoke =
                [&authoringCommands]
                {
                    authoringCommands.Invoke(
                        orbit::editor_model::
                            authoring_commands::
                                kRedo);
                },
            .enabled =
                [&authoringCommands]
                {
                    return authoringCommands.
                        Enablement(
                            orbit::editor_model::
                                authoring_commands::
                                    kRedo).
                        enabled;
                }
        });

        SynchronizePluginPanels(
            ui,
            plugins,
            pluginPanelIds);
        pluginPanelRevision =
            plugins.PanelCatalogRevision();

        orbit::log::Info(
            std::format(
                "Opened project '{}' ({})",
                project.Manifest().
                    displayName,
                project.Manifest().
                    projectId.ToString()));

        auto allocator =
            device.CreateCommandAllocator(
                orbit::rhi::QueueType::
                    Graphics);

        auto commands =
            device.CreateCommandList(
                *allocator);

        auto fence =
            device.CreateFence(0);

        orbit::u64 submittedFence = 0;
        orbit::u64 nextFence = 1;

        using Clock =
            std::chrono::steady_clock;

        auto previous =
            Clock::now();

        while (window.PumpEvents())
        {
            if (submittedFence != 0)
            {
                fence->Wait(
                    submittedFence);
            }

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
                continue;
            }

            static_cast<void>(
                runtime.
                    ResizeSwapchainToWindow());

            rpcServer.Poll();

            pluginReloadAccumulator +=
                deltaSeconds;

            if (pluginReloadAccumulator >=
                0.5)
            {
                const orbit::u32 reloaded =
                    plugins.PollHotReload();

                if (reloaded != 0)
                {
                    orbit::log::Info(
                        std::format(
                            "Hot reloaded {} plugin{}.",
                            reloaded,
                            reloaded == 1
                                ? ""
                                : "s"));

                    editorRpc.PublishEvent(
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

            if (plugins.PanelCatalogRevision() !=
                pluginPanelRevision)
            {
                SynchronizePluginPanels(
                    ui,
                    plugins,
                    pluginPanelIds);
                pluginPanelRevision =
                    plugins.
                        PanelCatalogRevision();
            }

            publishAutomationChanges();

            ui.BeginFrame(
                window,
                deltaSeconds);

            ui.DrawStudioShell();

            shortcuts.Update(
                window,
                authoringCommands,
                ui.WantsKeyboard());

            publishAutomationChanges();

            if (window.KeyDown(
                    orbit::platform::
                        Key::Escape) &&
                !ui.WantsKeyboard())
            {
                break;
            }

            allocator->Reset();
            commands->Reset(
                *allocator);

            auto& backBuffer =
                swapchain.
                    CurrentBackBuffer();

            orbit::render_graph::
                RenderGraph graph(device);

            const auto viewTargets =
                bodyView.Import(
                    graph,
                    "StudioBody");

            const auto backBufferTarget =
                graph.ImportTexture(
                    "StudioSwapchain",
                    backBuffer,
                    orbit::rhi::
                        ResourceState::
                            Present);

            const orbit::universe::BodyShape
                previewShape =
                    orbit::universe::
                        SphereShape{
                            .radiusMeters =
                                BodyRadius(
                                    objects,
                                    bodyObject)
                        };

            graph.AddPass(
                "Studio.BodyPreview",
                {
                    {
                        .texture =
                            viewTargets.color,
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
                    bodyPreview.Draw(
                        commandList,
                        bodyView.Color(),
                        bodyView.Width(),
                        bodyView.Height(),
                        previewShape,
                        bodyView.Camera());
                });

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

            graph.AddPass(
                "Studio.Ui",
                {
                    {
                        .texture =
                            viewTargets.color,
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
                },
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

            graph.Execute(*commands);

            commands->Close();
            graphicsQueue.Submit(
                *commands);

            swapchain.Present(true);

            submittedFence =
                nextFence++;

            graphicsQueue.Signal(
                *fence,
                submittedFence);
        }

        if (submittedFence != 0)
        {
            fence->Wait(
                submittedFence);
        }

        world.Checkpoint();
        return 0;
    }
    catch (const std::exception& exception)
    {
        orbit::log::Error(
            exception.what());
        return 1;
    }
}
