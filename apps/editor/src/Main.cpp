#include <orbit/build/BuildService.hpp>
#include <orbit/commands/CommandRegistry.hpp>
#include <orbit/commands/CommandService.hpp>
#include <orbit/content/ContentService.hpp>
#include <orbit/content/RuntimeTexture.hpp>
#include <orbit/content_wic/WicTextureImporter.hpp>
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
#include <orbit/editor_ui/PathPreviewRenderer.hpp>
#include <orbit/frames/FrameGraph.hpp>
#include <orbit/jobs/JobSystem.hpp>
#include <orbit/path_geometry/PathDerived.hpp>
#include <orbit/path_geometry/PathSource.hpp>
#include <orbit/path_routing/RouteDomains.hpp>
#include <orbit/path_routing/RoutePlanner.hpp>
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
#include <orbit/studio_ui/ProjectAuthoringUi.hpp>
#include <orbit/studio_ui/ProjectSettingsUi.hpp>
#include <orbit/studio_ui/StudioRenderViewSet.hpp>
#include <orbit/studio_ui/StudioViewportPanels.hpp>
#include <orbit/studio_ui/StudioViewportRenderer.hpp>
#include <orbit/studio_ui/SurfaceAuthoringUi.hpp>
#include <orbit/studio_ui/WorldDocumentsUi.hpp>
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
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace
{
[[nodiscard]] orbit::u64
ContentRevisionKey(
    const orbit::content::ContentHash& hash)
    noexcept
{
    orbit::u64 value =
        0xcbf29ce484222325ULL;

    for (const std::byte byte :
         hash.Bytes())
    {
        value ^=
            static_cast<orbit::u64>(
                std::to_integer<
                    orbit::u8>(byte));
        value *=
            0x100000001b3ULL;
    }

    return value;
}

struct ResolvedRoutingProfile
{
    orbit::paths::PathProfile profile;
    orbit::u64 revision{1};
};

[[nodiscard]] ResolvedRoutingProfile
ResolveRoutingProfile(
    const orbit::paths::PathEdgeRecord& edge,
    orbit::paths::PathNetworkService& paths,
    const orbit::content::ContentService& content,
    const std::filesystem::path& projectRoot)
{
    const auto network =
        paths.FindNetwork(
            edge.network);

    if (!network.has_value())
    {
        throw std::runtime_error(
            "Routed edge references an unknown path network.");
    }

    const std::string assetText =
        edge.profileOverride.empty()
            ? network->profileAsset
            : edge.profileOverride;

    if (assetText.empty())
    {
        return {
            .profile = {
                .name = "Default Road",
                .kind =
                    orbit::paths::
                        PathProfileKind::Road
            },
            .revision = 1
        };
    }

    const auto assetId =
        orbit::content::AssetId::Parse(
            assetText);

    if (!assetId.has_value())
    {
        throw std::runtime_error(
            "Routed edge path profile is not a valid AssetId.");
    }

    const auto* asset =
        content.Find(
            *assetId);

    if (asset == nullptr ||
        asset->kind !=
            orbit::content::
                AssetKind::PathProfile)
    {
        throw std::runtime_error(
            "Routed edge path profile asset is missing or has the wrong kind.");
    }

    return {
        .profile =
            orbit::paths::LoadPathProfile(
                projectRoot /
                asset->sourcePath),
        .revision =
            ContentRevisionKey(
                asset->sourceHash)
    };
}

[[nodiscard]] std::vector<
    orbit::scene::ObjectId>
FindRoutedPathEdges(
    orbit::scene::ObjectStore& objects,
    orbit::paths::PathNetworkService& paths)
{
    std::vector<orbit::scene::ObjectRecord>
        pending =
            objects.Roots();
    std::vector<orbit::scene::ObjectId>
        result;

    while (!pending.empty())
    {
        const auto object =
            pending.back();
        pending.pop_back();

        if (object.type ==
            orbit::paths::kPathEdgeType)
        {
            const auto edge =
                paths.FindEdge(
                    object.id);

            if (edge.has_value() &&
                edge->mode ==
                    orbit::paths::
                        EdgeMode::Routed)
            {
                result.push_back(
                    object.id);
            }
        }

        auto children =
            objects.Children(
                object.id);

        pending.insert(
            pending.end(),
            children.begin(),
            children.end());
    }

    return result;
}

[[nodiscard]] std::vector<
    orbit::scene::ObjectId>
FindPathEdges(
    orbit::scene::ObjectStore& objects,
    orbit::paths::PathNetworkService& paths)
{
    std::vector<orbit::scene::ObjectRecord>
        pending =
            objects.Roots();
    std::vector<orbit::scene::ObjectId>
        result;

    while (!pending.empty())
    {
        const auto object =
            pending.back();
        pending.pop_back();

        if (object.type ==
            orbit::paths::kPathEdgeType &&
            paths.FindEdge(object.id).
                has_value())
        {
            result.push_back(
                object.id);
        }

        auto children =
            objects.Children(
                object.id);

        pending.insert(
            pending.end(),
            children.begin(),
            children.end());
    }

    return result;
}

[[nodiscard]] std::optional<
    orbit::frames::FrameId>
PathAnchorNativeFrame(
    const orbit::paths::PathAnchor& anchor,
    const orbit::universe::BodyRegistry& bodies)
{
    return std::visit(
        [&bodies](const auto& value)
            -> std::optional<
                orbit::frames::FrameId>
        {
            using Anchor =
                std::decay_t<
                    decltype(value)>;

            if constexpr (
                std::is_same_v<
                    Anchor,
                    orbit::paths::
                        FramePointAnchor>)
            {
                return value.frame;
            }
            else if constexpr (
                std::is_same_v<
                    Anchor,
                    orbit::paths::
                        SurfaceAnchor>)
            {
                const auto* body =
                    bodies.FindBody(
                        value.body);

                if (body == nullptr)
                {
                    return std::nullopt;
                }

                return body->frame;
            }
            else
            {
                // Entity/socket anchors require the owning entity runtime to
                // provide a frame. Studio does not invent one.
                return std::nullopt;
            }
        },
        anchor);
}

[[nodiscard]]
orbit::path_routing::RouteSearchConfig
RouteSearchForProfile(
    const orbit::paths::PathProfile& profile)
{
    const orbit::f64 spacing =
        std::max(
            5.0,
            profile.widthMeters * 2.0);

    return {
        .spacingMeters = spacing,
        .corridorHalfWidthMeters =
            std::max({
                100.0,
                spacing * 8.0,
                profile.
                    minimumRadiusMeters *
                    2.0
            }),
        .maximumAlongSamples = 256,
        .maximumLateralSamples = 33,
        .maximumGridCells = 8'192
    };
}

[[nodiscard]] std::optional<
    orbit::documents::ProjectDocument>
OpenProjectBrowser()
{
    orbit::studio_session::StudioWorkspace
        workspace;

    orbit::runtime::RuntimeSession runtime({
        .applicationName = "OrbitStudio",
        .windowTitle = "Orbit Studio - Project Browser",
        .width = 1180,
        .height = 760,
        .swapchainBufferCount = 3,
        .allowTearing = true,
        .relativeMouseMode = false
    });

    auto& window = runtime.Window();
    auto& device = runtime.Device();
    auto& graphicsQueue =
        runtime.GraphicsQueue();
    auto& swapchain =
        runtime.Swapchain();

    const orbit::shader::dxc::
        DxcShaderCompiler compiler;

    const auto layoutPath =
        orbit::platform::
            UserDataDirectory() /
        "EditorLayouts" /
        "ProjectBrowser.ini";

    orbit::editor_ui::EditorUi ui(
        device,
        graphicsQueue,
        compiler,
        layoutPath);

    orbit::studio_ui::ProjectAuthoringUi
        projectUi(
            workspace,
            orbit::platform::
                UserDataDirectory() /
                "RecentProjects.txt");

    projectUi.Register(ui);

    auto allocator =
        device.CreateCommandAllocator(
            orbit::rhi::QueueType::Graphics);
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

    std::optional<std::filesystem::path>
        selectedManifest;

    while (window.PumpEvents())
    {
        if (submittedFence != 0)
        {
            fence->Wait(
                submittedFence);
        }

        const auto now =
            Clock::now();
        const orbit::f64 deltaSeconds =
            std::clamp(
                std::chrono::duration<
                    orbit::f64>(
                        now - previous).
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
            runtime.ResizeSwapchainToWindow());

        ui.BeginFrame(
            window,
            deltaSeconds);
        ui.DrawStudioShell();

        if (workspace.HasProject())
        {
            workspace.Project().Save();
            workspace.Session().
                World().Checkpoint();

            selectedManifest =
                workspace.Project().
                    ManifestPath();
        }

        allocator->Reset();
        commands->Reset(
            *allocator);

        auto& backBuffer =
            swapchain.CurrentBackBuffer();

        orbit::render_graph::RenderGraph
            graph(device);

        const auto backBufferTarget =
            graph.ImportTexture(
                "ProjectBrowserSwapchain",
                backBuffer,
                orbit::rhi::
                    ResourceState::Present);

        graph.AddPass(
            "ProjectBrowser.Canvas",
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
                commandList.ClearColorTarget(
                    backBuffer,
                    {
                        .red = 0.018F,
                        .green = 0.021F,
                        .blue = 0.027F,
                        .alpha = 1.0F
                    });
                commandList.SetRenderTarget(
                    backBuffer);
            });

        graph.AddPass(
            "ProjectBrowser.Ui",
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
                ui.Render(
                    commandList,
                    backBuffer,
                    swapchain.Width(),
                    swapchain.Height());
            });

        graph.AddPass(
            "ProjectBrowser.Present",
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

        if (selectedManifest.has_value())
        {
            break;
        }
    }

    if (submittedFence != 0)
    {
        fence->Wait(
            submittedFence);
    }

    if (!selectedManifest.has_value())
    {
        return std::nullopt;
    }

    // Destroy the browser workspace/session graph before opening the selected
    // project as the editor's authoritative project graph.
    workspace.CloseProject();

    return orbit::documents::
        ProjectDocument::Open(
            *selectedManifest);
}

[[nodiscard]] std::optional<
    orbit::documents::ProjectDocument>
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

    // Normal Studio launches now use the real project browser instead of
    // silently manufacturing/opening a hidden scratch project.
    return OpenProjectBrowser();
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
    const auto existingBody =
        FindFirstBodyObject(objects);

    // Legacy preview projects created the first body directly under World.
    // M20A authority requires World -> Celestial System -> Celestial Body.
    // Normalize that hierarchy through ordinary undoable semantic commands
    // instead of teaching UniverseComposition an editor-only exception.
    if (existingBody.has_value())
    {
        const auto bodyRecord =
            objects.Find(*existingBody);

        if (!bodyRecord.has_value())
        {
            throw std::runtime_error(
                "Studio initial body disappeared during hierarchy inspection.");
        }

        if (bodyRecord->parent.has_value())
        {
            const auto parent =
                objects.Find(*bodyRecord->parent);

            if (parent.has_value() &&
                parent->type ==
                    orbit::editor_model::builtin::
                        kCelestialSystemType)
            {
                return *existingBody;
            }
        }
    }

    commands.BeginTransaction(
        existingBody.has_value()
            ? "Migrate Initial World Hierarchy"
            : "Initialize World");

    try
    {
        const auto roots =
            objects.Roots();

        orbit::scene::ObjectId worldRoot{};

        for (const auto& root : roots)
        {
            if (root.type ==
                orbit::editor_model::builtin::
                    kWorldType)
            {
                worldRoot = root.id;
                break;
            }
        }

        if (!worldRoot)
        {
            worldRoot =
                commands.CreateObject(
                    orbit::editor_model::
                        builtin::kWorldType,
                    "World");
        }

        orbit::scene::ObjectId systemObject{};

        for (const auto& child :
             objects.Children(worldRoot))
        {
            if (child.type ==
                orbit::editor_model::builtin::
                    kCelestialSystemType)
            {
                systemObject = child.id;
                break;
            }
        }

        if (!systemObject)
        {
            systemObject =
                commands.CreateObject(
                    orbit::editor_model::
                        builtin::
                            kCelestialSystemType,
                    "Helion",
                    worldRoot);
        }

        orbit::scene::ObjectId body{};

        if (existingBody.has_value())
        {
            body = *existingBody;
            commands.ReparentObject(
                body,
                systemObject);
        }
        else
        {
            body =
                commands.CreateObject(
                    orbit::editor_model::
                        builtin::
                            kCelestialBodyType,
                    "Asterra",
                    systemObject);

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
        }

        commands.CommitTransaction();
        return body;
    }
    catch (...)
    {
        if (commands.HasActiveTransaction())
        {
            commands.RollbackTransaction();
        }
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
    const std::function<
        orbit::plugins::PluginManager&()>&
        plugins,
    std::vector<orbit::editor_ui::PanelId>&
        registered)
{
    const auto catalog =
        plugins().PanelCatalog();

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
                    try
                    {
                        if (!plugins().DrawPanel(
                                id,
                                context))
                        {
                            context.Text(
                                "Plugin panel is unavailable.");
                        }
                    }
                    catch (const std::logic_error&)
                    {
                        context.Text(
                            "No world is open.");
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

[[nodiscard]] orbit::math::Float3
AverageTextureColor(
    orbit::content::ContentService& content,
    const orbit::content::AssetRecord* texture)
{
    constexpr orbit::math::Float3 fallback{
        0.34F,
        0.37F,
        0.42F
    };

    if (texture == nullptr ||
        texture->kind != orbit::content::AssetKind::Texture ||
        !texture->derivedKey.has_value())
    {
        return fallback;
    }

    const auto bytes =
        content.Cache().Read(
            *texture->derivedKey,
            "texture.orbittex");

    if (!bytes.has_value())
    {
        return fallback;
    }

    try
    {
        const auto runtimeTexture =
            orbit::content::DecodeRuntimeTexture(
                std::span(
                    bytes->data(),
                    bytes->size()));

        const std::size_t pixelCount =
            runtimeTexture.pixels.size() / 4U;

        if (pixelCount == 0)
        {
            return fallback;
        }

        const std::size_t stride =
            std::max<std::size_t>(
                1U,
                pixelCount / 4096U);

        orbit::f64 red = 0.0;
        orbit::f64 green = 0.0;
        orbit::f64 blue = 0.0;
        std::size_t samples = 0;

        for (std::size_t pixel = 0;
             pixel < pixelCount;
             pixel += stride)
        {
            const std::size_t offset =
                pixel * 4U;

            red += std::to_integer<orbit::u8>(
                runtimeTexture.pixels[offset]);
            green += std::to_integer<orbit::u8>(
                runtimeTexture.pixels[offset + 1U]);
            blue += std::to_integer<orbit::u8>(
                runtimeTexture.pixels[offset + 2U]);
            ++samples;
        }

        const orbit::f32 scale =
            1.0F /
            static_cast<orbit::f32>(
                samples * 255U);

        return {
            static_cast<orbit::f32>(red) * scale,
            static_cast<orbit::f32>(green) * scale,
            static_cast<orbit::f32>(blue) * scale
        };
    }
    catch (const std::exception&)
    {
        return fallback;
    }
}

[[nodiscard]] orbit::editor_ui::PreviewMaterial
PreviewMaterialForAsset(
    orbit::content::ContentService& content,
    const orbit::content::AssetRecord* asset,
    const orbit::u32 depth = 0)
{
    orbit::editor_ui::PreviewMaterial result{};

    if (asset == nullptr || depth > 4U)
    {
        return result;
    }

    if (asset->kind == orbit::content::AssetKind::Material &&
        asset->material.has_value())
    {
        const auto& material = *asset->material;
        result.roughness = static_cast<orbit::f32>(
            std::clamp(material.roughnessFactor, 0.0, 1.0));
        result.metallic = static_cast<orbit::f32>(
            std::clamp(material.metallicFactor, 0.0, 1.0));

        if (!material.baseColor.empty())
        {
            const auto texturePath =
                asset->sourcePath.parent_path() /
                material.baseColor;
            result.baseColor =
                AverageTextureColor(
                    content,
                    content.FindByPath(texturePath));
        }

        return result;
    }

    if (asset->kind == orbit::content::AssetKind::MaterialInstance &&
        asset->materialInstance.has_value())
    {
        const auto& instance =
            *asset->materialInstance;
        const auto parentPath =
            asset->sourcePath.parent_path() /
            instance.parent;

        result = PreviewMaterialForAsset(
            content,
            content.FindByPath(parentPath),
            depth + 1U);

        if (instance.roughnessFactor.has_value())
        {
            result.roughness = static_cast<orbit::f32>(
                std::clamp(
                    *instance.roughnessFactor,
                    0.0,
                    1.0));
        }

        if (instance.metallicFactor.has_value())
        {
            result.metallic = static_cast<orbit::f32>(
                std::clamp(
                    *instance.metallicFactor,
                    0.0,
                    1.0));
        }

        return result;
    }

    if (asset->kind == orbit::content::AssetKind::Decal &&
        asset->decal.has_value())
    {
        const auto texturePath =
            asset->sourcePath.parent_path() /
            asset->decal->texture;
        result.baseColor =
            AverageTextureColor(
                content,
                content.FindByPath(texturePath));
        result.roughness = 0.55F;
        result.metallic = 0.0F;
    }

    return result;
}

[[nodiscard]] std::filesystem::path
FindPlayerExecutable()
{
    const auto executable =
        orbit::platform::
            ExecutablePath();

    const auto sibling =
        executable.parent_path() /
        "OrbitPlayer.exe";

    if (std::filesystem::
            is_regular_file(
                sibling))
    {
        return sibling;
    }

    const auto configuration =
        executable.parent_path().
            filename();

    const auto appsRoot =
        executable.parent_path().
            parent_path().
            parent_path();

    const auto development =
        appsRoot /
        "player" /
        configuration /
        "OrbitPlayer.exe";

    if (std::filesystem::
            is_regular_file(
                development))
    {
        return development;
    }

    throw std::runtime_error(
        "OrbitPlayer.exe was not found beside OrbitStudio or in the development build tree.");
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

        auto openedProject =
            OpenProject(
                argc,
                argv);

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

        orbit::dev_server::DevServer
            rpcServer({
                .port = 4320,
                .maxMessageBytes =
                    1024U * 1024U
            });

        rpcServer.SetMessageHandler(
            [&studioSession](
                const std::string_view message)
            {
                return studioSession.DispatchRpc(
                    message);
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
            bodyView(
                device,
                {
                    .width = 960,
                    .height = 640
                });

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
            const orbit::f64 initialBodyRadius =
                BodyRadius(
                    objects(),
                    bodyObject);

            bodyView.Camera().frame =
                initialBody->frame;
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
        }
        else
        {
            bodyView.Camera().
                localPositionMeters = {
                    0.0,
                    0.0,
                    -3.2
                };
            bodyView.Camera().nearPlaneMeters =
                0.01F;
            bodyView.Camera().farPlaneMeters =
                10.0F;
        }

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

        materialView.Camera().frame =
            bodyView.Camera().frame;
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

        orbit::editor_ui::
            PathPreviewRenderer
                pathPreview(
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

        orbit::studio_session::StudioRuntimeBinding
            studioRuntime(
                studioSession);

        orbit::studio_ui::StudioRenderViewSet
            studioViews(
                device,
                studioSession);

        orbit::studio_ui::StudioViewportPanels
            studioViewportPanels(
                studioViews,
                studioSession);

        orbit::studio_ui::StudioViewportRenderer
            studioViewportRenderer(
                device,
                compiler,
                swapchain.BufferCount());

        auto* primaryStudioView =
            studioViews.Find(
                "studio.primary");

        if (primaryStudioView == nullptr)
        {
            throw std::logic_error(
                "Studio primary RenderView was not created.");
        }

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
                        CaptureBmp(
                            device,
                            graphicsQueue,
                            *primaryStudioView,
                            path);
                }
        });

        studioViewportPanels.
            RegisterSecondary(ui);

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

        orbit::studio_ui::SurfaceAuthoringUi
            surfaceAuthoringUi(
                studioSession);
        surfaceAuthoringUi.Register(ui);

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
            kBuildPanel{
                .high =
                    0x4f52424954535455ULL,
                .low =
                    0x44494f4255494c44ULL
            };

        constexpr orbit::editor_ui::PanelId
            kPlatformServicesPanel{
                .high =
                    0x4f52424954535455ULL,
                .low =
                    0x44494f504c415446ULL
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
        std::optional<orbit::content::AssetId>
            materialPreviewAsset;
        orbit::editor_ui::PreviewMaterial
            materialPreviewMaterial{};
        std::string renameBuffer;
        orbit::build::BuildService
            buildService;
        std::string selectedBuildProfile =
            project.Manifest().
                    buildProfiles.empty()
                ? std::string{}
                : project.Manifest().
                      buildProfiles.front().
                      name;
        std::vector<orbit::build::BuildIssue>
            buildIssues;
        std::filesystem::path
            lastBuildManifest;
        std::filesystem::path
            lastPackageExecutable;
        std::string buildStatus{
            "Not run"};

        const std::filesystem::path
            platformConfigurationPath =
                project.RootDirectory() /
                "Config" /
                "PlatformServices.toml";
        orbit::platform_services::
            PlatformConfiguration
                platformConfiguration;
        std::vector<
            orbit::platform_services::
                PlatformConfigIssue>
            platformConfigurationIssues;
        std::string platformStatus{
            "Not configured"};
        orbit::i64 steamAppIdEditor = 0;
        std::string newAchievementId;
        std::string newAchievementApiName;
        std::string newStatId;
        std::string newStatApiName;
        bool newStatFloat = false;
        std::string newTimelineEventId;
        std::string newTimelineTitle;
        std::string newTimelineDescription;
        std::string newTimelineIcon{
            "steam_marker"};

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
        bool pathDebugVisualization = true;
        std::optional<orbit::paths::NetworkId>
            activePathNetwork;
        std::optional<orbit::scene::ObjectId>
            lastPlacedPathNode;

        const auto validateProjectBuild =
            [&]
            {
                try
                {
                    const auto result =
                        buildService.Validate({
                            .manifestPath =
                                project.ManifestPath(),
                            .profileName =
                                selectedBuildProfile
                        });

                    buildIssues =
                        result.issues;

                    if (result.Succeeded())
                    {
                        buildStatus =
                            std::format(
                                "Validated {} / {} / {}",
                                result.profile.platform,
                                result.profile.configuration,
                                result.profile.storefront);

                        orbit::log::Info(
                            std::format(
                                "Build validation succeeded for profile '{}'.",
                                result.profile.name));
                    }
                    else
                    {
                        buildStatus =
                            "Validation failed";
                        orbit::log::Error(
                            "Project build validation failed.");
                    }
                }
                catch (const std::exception&
                           exception)
                {
                    buildIssues = {
                        {
                            .severity =
                                orbit::build::
                                    IssueSeverity::Error,
                            .code =
                                "studio.build.exception",
                            .message =
                                exception.what()
                        }
                    };
                    buildStatus =
                        "Validation failed";
                    orbit::log::Error(
                        exception.what());
                }
            };

        const auto cookProjectBuild =
            [&]
            {
                try
                {
                    // Studio builds always package the currently authored
                    // semantic state, never a stale pre-edit checkpoint.
                    project.Save();
                    if (worldSession.HasWorld())
                    {
                        world().Checkpoint();
                    }

                    const auto result =
                        buildService.Cook({
                            .manifestPath =
                                project.ManifestPath(),
                            .profileName =
                                selectedBuildProfile
                        });

                    buildIssues =
                        result.issues;

                    if (result.Succeeded())
                    {
                        lastBuildManifest =
                            result.manifestPath;
                        buildStatus =
                            std::format(
                                "Cooked {} asset{} and {} script{}",
                                result.manifest.
                                    assets.size(),
                                result.manifest.
                                    assets.size() == 1U
                                    ? ""
                                    : "s",
                                result.manifest.
                                    scripts.size(),
                                result.manifest.
                                    scripts.size() == 1U
                                    ? ""
                                    : "s");

                        orbit::log::Info(
                            std::format(
                                "Build cook succeeded: {}",
                                result.manifestPath.
                                    string()));
                    }
                    else
                    {
                        buildStatus =
                            "Cook failed";
                        orbit::log::Error(
                            "Project build cook failed.");
                    }
                }
                catch (const std::exception&
                           exception)
                {
                    buildIssues = {
                        {
                            .severity =
                                orbit::build::
                                    IssueSeverity::Error,
                            .code =
                                "studio.build.exception",
                            .message =
                                exception.what()
                        }
                    };
                    buildStatus =
                        "Cook failed";
                    orbit::log::Error(
                        exception.what());
                }
            };

        const auto packageProjectBuild =
            [&](const std::string&
                    requestedProfile)
                -> orbit::build::
                    PackageResult
            {
                project.Save();
                if (worldSession.HasWorld())
                {
                    world().Checkpoint();
                }

                const auto result =
                    buildService.Package(
                        {
                            .manifestPath =
                                project.ManifestPath(),
                            .profileName =
                                requestedProfile
                        },
                        {
                            .playerExecutable =
                                FindPlayerExecutable()
                        });

                buildIssues =
                    result.issues;

                if (result.Succeeded())
                {
                    selectedBuildProfile =
                        result.manifest.
                            profile.name;
                    lastBuildManifest =
                        result.manifestPath;
                    lastPackageExecutable =
                        result.executablePath;
                    buildStatus =
                        std::format(
                            "Packaged {}",
                            result.executablePath.
                                filename().
                                string());

                    orbit::log::Info(
                        std::format(
                            "Project package succeeded: {}",
                            result.outputDirectory.
                                string()));
                }
                else
                {
                    buildStatus =
                        "Package failed";
                    orbit::log::Error(
                        "Project package failed.");
                }

                return result;
            };

        const auto buildIssuesToRpc =
            [](
                const std::vector<
                    orbit::build::BuildIssue>&
                    issues)
            {
                orbit::rpc::Value::Array
                    result;

                result.reserve(
                    issues.size());

                for (const auto& issue :
                     issues)
                {
                    result.emplace_back(
                        orbit::rpc::Value::Object{
                            {
                                "severity",
                                issue.severity ==
                                        orbit::build::
                                            IssueSeverity::Error
                                    ? "error"
                                    : "warning"
                            },
                            {"code", issue.code},
                            {
                                "message",
                                issue.message
                            },
                            {
                                "path",
                                issue.path.
                                    generic_string()
                            }
                        });
                }

                return orbit::rpc::Value(
                    std::move(result));
            };

        rpcHost.AttachBuild({
            .profiles =
                [&project]
                {
                    orbit::rpc::Value::Array
                        profiles;

                    for (const auto& profile :
                         project.Manifest().
                             buildProfiles)
                    {
                        profiles.emplace_back(
                            orbit::rpc::Value::Object{
                                {
                                    "name",
                                    profile.name
                                },
                                {
                                    "configuration",
                                    profile.
                                        configuration
                                },
                                {
                                    "platform",
                                    profile.platform
                                },
                                {
                                    "storefront",
                                    profile.storefront
                                }
                            });
                    }

                    return orbit::rpc::Value(
                        std::move(
                            profiles));
                },
            .validate =
                [&buildService,
                 &project,
                 &buildIssues,
                 &buildStatus,
                 &selectedBuildProfile,
                 buildIssuesToRpc](
                    std::optional<
                        std::string>
                        profile)
                {
                    const std::string
                        requestedProfile =
                            profile.
                                value_or(
                                    std::string{});

                    const auto result =
                        buildService.Validate({
                            .manifestPath =
                                project.
                                    ManifestPath(),
                            .profileName =
                                requestedProfile
                        });

                    buildIssues =
                        result.issues;

                    if (result.Succeeded())
                    {
                        selectedBuildProfile =
                            result.profile.name;
                        buildStatus =
                            std::format(
                                "Validated {} / {} / {}",
                                result.profile.platform,
                                result.profile.
                                    configuration,
                                result.profile.
                                    storefront);
                    }
                    else
                    {
                        buildStatus =
                            "Validation failed";
                    }

                    return orbit::rpc::Value(
                        orbit::rpc::Value::Object{
                            {
                                "ok",
                                result.Succeeded()
                            },
                            {
                                "profile",
                                result.profile.name
                            },
                            {
                                "configuration",
                                result.profile.
                                    configuration
                            },
                            {
                                "platform",
                                result.profile.
                                    platform
                            },
                            {
                                "storefront",
                                result.profile.
                                    storefront
                            },
                            {
                                "output",
                                result.
                                    outputDirectory.
                                    generic_string()
                            },
                            {
                                "issues",
                                buildIssuesToRpc(
                                    result.issues)
                            }
                        });
                },
            .cook =
                [&buildService,
                 &project,
                 &world,
                 &worldSession,
                 &buildIssues,
                 &buildStatus,
                 &selectedBuildProfile,
                 &lastBuildManifest,
                 buildIssuesToRpc](
                    std::optional<
                        std::string>
                        profile)
                {
                    try
                    {
                        project.Save();
                        if (worldSession.HasWorld())
                    {
                        world().Checkpoint();
                    }

                        const std::string
                            requestedProfile =
                                profile.
                                    value_or(
                                        std::string{});

                        const auto result =
                            buildService.Cook({
                                .manifestPath =
                                    project.
                                        ManifestPath(),
                                .profileName =
                                    requestedProfile
                            });

                        buildIssues =
                            result.issues;

                        if (result.Succeeded())
                        {
                            selectedBuildProfile =
                                result.manifest.
                                    profile.name;
                            lastBuildManifest =
                                result.manifestPath;
                            buildStatus =
                                std::format(
                                    "Cooked {} asset{} and {} script{}",
                                    result.manifest.
                                        assets.size(),
                                    result.manifest.
                                        assets.size() == 1U
                                        ? ""
                                        : "s",
                                    result.manifest.
                                        scripts.size(),
                                    result.manifest.
                                        scripts.size() == 1U
                                        ? ""
                                        : "s");
                        }
                        else
                        {
                            buildStatus =
                                "Cook failed";
                        }

                        return orbit::rpc::Value(
                            orbit::rpc::Value::Object{
                                {
                                    "ok",
                                    result.
                                        Succeeded()
                                },
                                {
                                    "profile",
                                    result.manifest.
                                        profile.name
                                },
                                {
                                    "output",
                                    result.
                                        outputDirectory.
                                        generic_string()
                                },
                                {
                                    "manifest",
                                    result.
                                        manifestPath.
                                        generic_string()
                                },
                                {
                                    "assets",
                                    static_cast<
                                        orbit::i64>(
                                            result.
                                                manifest.
                                                assets.
                                                size())
                                },
                                {
                                    "scripts",
                                    static_cast<
                                        orbit::i64>(
                                            result.
                                                manifest.
                                                scripts.
                                                size())
                                },
                                {
                                    "issues",
                                    buildIssuesToRpc(
                                        result.
                                            issues)
                                }
                            });
                    }
                    catch (const std::exception&
                               exception)
                    {
                        throw orbit::rpc::Error(
                            1100,
                            exception.what());
                    }
                },
            .package =
                [&packageProjectBuild,
                 buildIssuesToRpc](
                    std::optional<
                        std::string>
                        profile)
                {
                    try
                    {
                        const auto result =
                            packageProjectBuild(
                                profile.
                                    value_or(
                                        std::string{}));

                        return orbit::rpc::Value(
                            orbit::rpc::Value::Object{
                                {
                                    "ok",
                                    result.
                                        Succeeded()
                                },
                                {
                                    "profile",
                                    result.manifest.
                                        profile.name
                                },
                                {
                                    "output",
                                    result.
                                        outputDirectory.
                                        generic_string()
                                },
                                {
                                    "executable",
                                    result.
                                        executablePath.
                                        generic_string()
                                },
                                {
                                    "build_manifest",
                                    result.
                                        manifestPath.
                                        generic_string()
                                },
                                {
                                    "package_manifest",
                                    result.
                                        packageManifestPath.
                                        generic_string()
                                },
                                {
                                    "issues",
                                    buildIssuesToRpc(
                                        result.issues)
                                }
                            });
                    }
                    catch (const std::exception&
                               exception)
                    {
                        throw orbit::rpc::Error(
                            1101,
                            exception.what());
                    }
                }
        });

        // Route planning is owned by StudioSession and rebound whenever the
        // composed universe generation changes. Resolve it on use so no
        // reference survives a generation swap.
        const auto routePlanner =
            [&studioSession]()
                -> orbit::path_routing::RoutePlanner&
            {
                return studioSession.
                    PathRouting().Planner();
            };

        std::unordered_set<
            orbit::scene::ObjectId>
            knownRoutedEdges;
        std::unordered_map<
            orbit::scene::ObjectId,
            orbit::u64>
            publishedRouteGeneration;

        orbit::u64 routedObjectRevision =
            ~orbit::u64{0};
        orbit::u64 routedContentRevision =
            ~orbit::u64{0};

        std::unordered_map<
            orbit::scene::ObjectId,
            orbit::path_geometry::
                PathDerivedProduct>
            derivedPaths;
        std::unordered_map<
            orbit::scene::ObjectId,
            orbit::u64>
            derivedRouteGeneration;
        orbit::u64 derivedObjectRevision =
            ~orbit::u64{0};
        orbit::u64 derivedContentRevision =
            ~orbit::u64{0};

        const auto requestRoutedPaths =
            [&]
            {
                auto& pathService =
                    studioSession.PathNetwork().Service();

                const auto routedEdges =
                    FindRoutedPathEdges(
                        objects(),
                        pathService);

                std::unordered_set<
                    orbit::scene::ObjectId>
                    liveEdges(
                        routedEdges.begin(),
                        routedEdges.end());

                for (const auto edge :
                     knownRoutedEdges)
                {
                    if (!liveEdges.
                            contains(edge))
                    {
                        routePlanner().Erase(edge);
                        publishedRouteGeneration.
                            erase(edge);
                    }
                }

                for (const auto edgeId :
                     routedEdges)
                {
                    try
                    {
                        const auto edge =
                            pathService.FindEdge(
                                edgeId);

                        if (!edge.has_value())
                        {
                            continue;
                        }

                        const auto start =
                            pathService.FindNode(
                                edge->startNode);
                        const auto end =
                            pathService.FindNode(
                                edge->endNode);

                        if (!start.has_value() ||
                            !end.has_value())
                        {
                            throw std::runtime_error(
                                "Routed edge has an invalid endpoint.");
                        }

                        const auto resolvedProfile =
                            ResolveRoutingProfile(
                                *edge,
                                pathService,
                                content,
                                project.
                                    RootDirectory());

                        orbit::path_routing::
                            RouteEnvironment
                                environment;

                        environment.search =
                            RouteSearchForProfile(
                                resolvedProfile.
                                    profile);

                        const auto* startSurface =
                            std::get_if<
                                orbit::paths::
                                    SurfaceAnchor>(
                                        &start->
                                            anchor);
                        const auto* endSurface =
                            std::get_if<
                                orbit::paths::
                                    SurfaceAnchor>(
                                        &end->
                                            anchor);

                        if (startSurface !=
                                nullptr &&
                            endSurface !=
                                nullptr &&
                            startSurface->body ==
                                endSurface->body)
                        {
                            const auto* routeBody =
                                bodies().FindBody(
                                    startSurface->
                                        body);

                            if (routeBody ==
                                nullptr)
                            {
                                throw std::runtime_error(
                                    "Routed surface edge references an unknown runtime body.");
                            }

                            environment =
                                orbit::path_routing::
                                    MakeReferenceSurfaceEnvironment(
                                        startSurface->
                                            body,
                                        routeBody->
                                            frame,
                                        {},
                                        frames(),
                                        bodies(),
                                        RouteSearchForProfile(
                                            resolvedProfile.
                                                profile));
                        }

                        static_cast<void>(
                            routePlanner().Request({
                                .edge = *edge,
                                .startNode =
                                    *start,
                                .endNode =
                                    *end,
                                .profile =
                                    resolvedProfile.
                                        profile,
                                .profileRevision =
                                    resolvedProfile.
                                        revision,
                                .environment =
                                    std::move(
                                        environment)
                            }));
                    }
                    catch (const std::exception&
                               exception)
                    {
                        routePlanner().Erase(
                            edgeId);

                        orbit::log::Warning(
                            std::format(
                                "Route '{}': {}",
                                edgeId.ToString(),
                                exception.what()));
                    }
                }

                knownRoutedEdges =
                    std::move(liveEdges);
                routedObjectRevision =
                    objects().Revision();
                routedContentRevision =
                    content.Revision();
            };

        const auto pollRoutedPaths =
            [&]
            {
                if (objects().Revision() !=
                        routedObjectRevision ||
                    content.Revision() !=
                        routedContentRevision)
                {
                    requestRoutedPaths();
                }

                routePlanner().Poll();

                for (const auto edge :
                     knownRoutedEdges)
                {
                    const auto status =
                        routePlanner().Status(edge);

                    if (!status.has_value())
                    {
                        continue;
                    }

                    const auto published =
                        publishedRouteGeneration.
                            find(edge);

                    if (published !=
                            publishedRouteGeneration.
                                end() &&
                        published->second ==
                            status->generation)
                    {
                        continue;
                    }

                    if (status->state ==
                        orbit::path_routing::
                            RouteState::Ready)
                    {
                        const auto* result =
                            routePlanner().Result(edge);

                        if (result == nullptr)
                        {
                            continue;
                        }

                        publishedRouteGeneration[
                            edge] =
                                status->
                                    generation;

                        rpcHost.PublishEvent(
                            "path.route_ready",
                            orbit::rpc::Value(
                                orbit::rpc::Value::Object{
                                    {
                                        "edge",
                                        edge.ToString()
                                    },
                                    {
                                        "generation",
                                        static_cast<
                                            orbit::i64>(
                                                status->
                                                    generation)
                                    },
                                    {
                                        "revision",
                                        static_cast<
                                            orbit::i64>(
                                                status->
                                                    committedRevision)
                                    },
                                    {
                                        "points",
                                        static_cast<
                                            orbit::i64>(
                                                result->
                                                    points.
                                                    size())
                                    },
                                    {
                                        "total_cost",
                                        result->
                                            totalCost
                                    },
                                    {
                                        "cross_frame",
                                        result->
                                            crossFrameEndpoints
                                    }
                                }));
                    }
                    else if (
                        status->state ==
                            orbit::path_routing::
                                RouteState::Failed)
                    {
                        publishedRouteGeneration[
                            edge] =
                                status->
                                    generation;

                        orbit::log::Warning(
                            std::format(
                                "Route '{}' failed: {}",
                                edge.ToString(),
                                status->error));

                        rpcHost.PublishEvent(
                            "path.route_failed",
                            orbit::rpc::Value(
                                orbit::rpc::Value::Object{
                                    {
                                        "edge",
                                        edge.ToString()
                                    },
                                    {
                                        "generation",
                                        static_cast<
                                            orbit::i64>(
                                                status->
                                                    generation)
                                    },
                                    {
                                        "error",
                                        status->error
                                    }
                                }));
                    }
                }
            };

        const auto refreshDerivedPaths =
            [&]
            {
                bool requiresRefresh =
                    objects().Revision() !=
                        derivedObjectRevision ||
                    content.Revision() !=
                        derivedContentRevision;

                for (const auto edge :
                     knownRoutedEdges)
                {
                    const auto status =
                        routePlanner().Status(edge);

                    if (!status.has_value() ||
                        status->state !=
                            orbit::path_routing::
                                RouteState::Ready)
                    {
                        continue;
                    }

                    const auto built =
                        derivedRouteGeneration.
                            find(edge);

                    if (built ==
                            derivedRouteGeneration.
                                end() ||
                        built->second !=
                            status->generation)
                    {
                        requiresRefresh = true;
                        break;
                    }
                }

                if (!requiresRefresh)
                {
                    return;
                }

                auto& pathService =
                    studioSession.PathNetwork().Service();

                const auto edgeIds =
                    FindPathEdges(
                        objects(),
                        pathService);

                std::unordered_set<
                    orbit::scene::ObjectId>
                    liveEdges(
                        edgeIds.begin(),
                        edgeIds.end());

                for (auto item =
                         derivedPaths.begin();
                     item !=
                         derivedPaths.end();)
                {
                    if (!liveEdges.contains(
                            item->first))
                    {
                        derivedRouteGeneration.
                            erase(item->first);
                        item =
                            derivedPaths.erase(
                                item);
                    }
                    else
                    {
                        ++item;
                    }
                }

                for (const auto edgeId :
                     edgeIds)
                {
                    try
                    {
                        const auto edge =
                            pathService.FindEdge(
                                edgeId);

                        if (!edge.has_value())
                        {
                            continue;
                        }

                        const auto start =
                            pathService.FindNode(
                                edge->startNode);
                        const auto end =
                            pathService.FindNode(
                                edge->endNode);

                        if (!start.has_value() ||
                            !end.has_value())
                        {
                            throw std::runtime_error(
                                "Path geometry edge has an invalid endpoint.");
                        }

                        const auto resolvedProfile =
                            ResolveRoutingProfile(
                                *edge,
                                pathService,
                                content,
                                project.
                                    RootDirectory());

                        orbit::frames::FrameId
                            targetFrame{};
                        const orbit::path_routing::
                            RouteResult*
                                routeResult =
                                    nullptr;
                        orbit::u64 routeGeneration =
                            0;

                        if (edge->mode ==
                            orbit::paths::
                                EdgeMode::Routed)
                        {
                            const auto status =
                                routePlanner().Status(
                                    edgeId);

                            if (!status.has_value() ||
                                status->state !=
                                    orbit::path_routing::
                                        RouteState::Ready)
                            {
                                continue;
                            }

                            routeResult =
                                routePlanner().Result(
                                    edgeId);

                            if (routeResult ==
                                nullptr)
                            {
                                continue;
                            }

                            targetFrame =
                                routeResult->frame;
                            routeGeneration =
                                status->generation;
                        }
                        else
                        {
                            const auto startFrame =
                                PathAnchorNativeFrame(
                                    start->anchor,
                                    bodies());
                            const auto endFrame =
                                PathAnchorNativeFrame(
                                    end->anchor,
                                    bodies());

                            targetFrame =
                                startFrame.
                                    value_or(
                                        endFrame.
                                            value_or(
                                                orbit::frames::
                                                    FrameId{}));

                            if (!targetFrame)
                            {
                                continue;
                            }
                        }

                        std::string failure;

                        const orbit::f64
                            sampleSpacing =
                                std::clamp(
                                    resolvedProfile.
                                        profile.
                                        widthMeters *
                                        0.5,
                                    1.0,
                                    4.0);

                        const auto centerline =
                            orbit::path_geometry::
                                BuildPathCenterline({
                                    .edge = *edge,
                                    .startNode = *start,
                                    .endNode = *end,
                                    .targetFrame =
                                        targetFrame,
                                    .frames = &frames(),
                                    .bodies = &bodies(),
                                    .routed =
                                        routeResult,
                                    .curveSampleSpacingMeters =
                                        sampleSpacing
                                },
                                &failure);

                        if (!centerline.has_value())
                        {
                            throw std::runtime_error(
                                failure.empty()
                                    ? "Path centerline derivation failed."
                                    : failure);
                        }

                        auto product =
                            orbit::path_geometry::
                                BuildPathDerived(
                                    *centerline,
                                    resolvedProfile.
                                        profile,
                                    {
                                        .sampleSpacingMeters =
                                            sampleSpacing
                                    });

                        const auto existing =
                            derivedPaths.find(
                                edgeId);

                        const bool changed =
                            existing ==
                                derivedPaths.end() ||
                            existing->second.
                                buildSignature !=
                                product.
                                    buildSignature;

                        if (changed)
                        {
                            derivedPaths[
                                edgeId] =
                                    std::move(
                                        product);

                            const auto& committed =
                                derivedPaths.at(
                                    edgeId);

                            rpcHost.PublishEvent(
                                "path.derived_ready",
                                orbit::rpc::Value(
                                    orbit::rpc::Value::Object{
                                        {
                                            "edge",
                                            edgeId.
                                                ToString()
                                        },
                                        {
                                            "signature",
                                            static_cast<
                                                orbit::i64>(
                                                    committed.
                                                        buildSignature &
                                                    0x7fffffffffffffffULL)
                                        },
                                        {
                                            "vertices",
                                            static_cast<
                                                orbit::i64>(
                                                    committed.
                                                        visualMesh.
                                                        vertices.
                                                        size())
                                        },
                                        {
                                            "lanes",
                                            static_cast<
                                                orbit::i64>(
                                                    committed.
                                                        lanes.
                                                        size())
                                        },
                                        {
                                            "nav_samples",
                                            static_cast<
                                                orbit::i64>(
                                                    committed.
                                                        navigation.
                                                        size())
                                        }
                                    }));
                        }

                        if (edge->mode ==
                            orbit::paths::
                                EdgeMode::Routed)
                        {
                            derivedRouteGeneration[
                                edgeId] =
                                    routeGeneration;
                        }
                    }
                    catch (const std::exception&
                               exception)
                    {
                        orbit::log::Warning(
                            std::format(
                                "Path geometry '{}': {}",
                                edgeId.ToString(),
                                exception.what()));
                    }
                }

                derivedObjectRevision =
                    objects().Revision();
                derivedContentRevision =
                    content.Revision();
            };

        rpcHost.AttachPathRouting({
            .status =
                [&routePlanner](
                    const orbit::scene::ObjectId edge)
                {
                    const auto status =
                        routePlanner().Status(edge);

                    if (!status.has_value())
                    {
                        return orbit::rpc::Value(
                            orbit::rpc::Value::Object{
                                {"state", "missing"},
                                {
                                    "edge",
                                    edge.ToString()
                                }
                            });
                    }

                    const char* stateName =
                        "missing";

                    switch (status->state)
                    {
                    case orbit::path_routing::
                            RouteState::Missing:
                        stateName = "missing";
                        break;
                    case orbit::path_routing::
                            RouteState::Dirty:
                        stateName = "dirty";
                        break;
                    case orbit::path_routing::
                            RouteState::Building:
                        stateName = "building";
                        break;
                    case orbit::path_routing::
                            RouteState::Ready:
                        stateName = "ready";
                        break;
                    case orbit::path_routing::
                            RouteState::Failed:
                        stateName = "failed";
                        break;
                    }

                    return orbit::rpc::Value(
                        orbit::rpc::Value::Object{
                            {
                                "edge",
                                edge.ToString()
                            },
                            {"state", stateName},
                            {
                                "generation",
                                static_cast<
                                    orbit::i64>(
                                        status->
                                            generation)
                            },
                            {
                                "committed_revision",
                                static_cast<
                                    orbit::i64>(
                                        status->
                                            committedRevision)
                            },
                            {
                                "dependency_signature",
                                static_cast<
                                    orbit::i64>(
                                        status->
                                            dependencySignature &
                                        0x7fffffffffffffffULL)
                            },
                            {
                                "cross_frame",
                                status->
                                    crossFrameEndpoints
                            },
                            {
                                "error",
                                status->error
                            }
                        });
                },
            .result =
                [&routePlanner](
                    const orbit::scene::ObjectId edge)
                {
                    const auto* result =
                        routePlanner().Result(edge);

                    if (result == nullptr)
                    {
                        return orbit::rpc::Value(
                            orbit::rpc::Value::Object{
                                {
                                    "edge",
                                    edge.ToString()
                                },
                                {"ready", false}
                            });
                    }

                    orbit::rpc::Value::Array
                        points;

                    points.reserve(
                        result->points.size());

                    for (const auto& point :
                         result->points)
                    {
                        points.emplace_back(
                            orbit::rpc::Value::Object{
                                {
                                    "position",
                                    orbit::rpc::Value::Array{
                                        point.localMeters.x,
                                        point.localMeters.y,
                                        point.localMeters.z
                                    }
                                },
                                {
                                    "elevation_meters",
                                    point.
                                        elevationMeters
                                },
                                {
                                    "water_depth_meters",
                                    point.
                                        waterDepthMeters
                                }
                            });
                    }

                    return orbit::rpc::Value(
                        orbit::rpc::Value::Object{
                            {
                                "edge",
                                edge.ToString()
                            },
                            {"ready", true},
                            {
                                "frame",
                                result->frame.
                                    ToString()
                            },
                            {
                                "generation",
                                static_cast<
                                    orbit::i64>(
                                        result->
                                            generation)
                            },
                            {
                                "dependency_signature",
                                static_cast<
                                    orbit::i64>(
                                        result->
                                            dependencySignature &
                                        0x7fffffffffffffffULL)
                            },
                            {
                                "total_cost",
                                result->totalCost
                            },
                            {
                                "cross_frame",
                                result->
                                    crossFrameEndpoints
                            },
                            {
                                "points",
                                std::move(points)
                            }
                        });
                },
            .invalidate =
                [&routePlanner](
                    const orbit::scene::ObjectId edge)
                {
                    routePlanner().Invalidate(edge);
                }
        });

        rpcHost.AttachPathGeometry({
            .result =
                [&derivedPaths](
                    const orbit::scene::ObjectId edge)
                {
                    const auto found =
                        derivedPaths.find(edge);

                    if (found ==
                        derivedPaths.end())
                    {
                        return orbit::rpc::Value(
                            orbit::rpc::Value::Object{
                                {
                                    "edge",
                                    edge.ToString()
                                },
                                {"ready", false}
                            });
                    }

                    const auto& product =
                        found->second;

                    orbit::rpc::Value::Array
                        lanes;
                    lanes.reserve(
                        product.lanes.size());

                    for (const auto& lane :
                         product.lanes)
                    {
                        orbit::rpc::Value::Array
                            points;
                        points.reserve(
                            lane.points.size());

                        for (const auto& point :
                             lane.points)
                        {
                            points.emplace_back(
                                orbit::rpc::Value::Object{
                                    {
                                        "station_meters",
                                        point.
                                            stationMeters
                                    },
                                    {
                                        "position",
                                        orbit::rpc::Value::Array{
                                            point.position.x,
                                            point.position.y,
                                            point.position.z
                                        }
                                    },
                                    {
                                        "tangent",
                                        orbit::rpc::Value::Array{
                                            point.tangent.x,
                                            point.tangent.y,
                                            point.tangent.z
                                        }
                                    }
                                });
                        }

                        lanes.emplace_back(
                            orbit::rpc::Value::Object{
                                {
                                    "lane",
                                    static_cast<
                                        orbit::i64>(
                                            lane.
                                                laneIndex)
                                },
                                {
                                    "lateral_offset_meters",
                                    lane.
                                        lateralOffsetMeters
                                },
                                {
                                    "points",
                                    std::move(points)
                                }
                            });
                    }

                    orbit::rpc::Value::Array
                        navigation;
                    navigation.reserve(
                        product.navigation.
                            size());

                    for (const auto& sample :
                         product.navigation)
                    {
                        navigation.emplace_back(
                            orbit::rpc::Value::Object{
                                {
                                    "station_meters",
                                    sample.
                                        stationMeters
                                },
                                {
                                    "position",
                                    orbit::rpc::Value::Array{
                                        sample.position.x,
                                        sample.position.y,
                                        sample.position.z
                                    }
                                },
                                {
                                    "tangent",
                                    orbit::rpc::Value::Array{
                                        sample.tangent.x,
                                        sample.tangent.y,
                                        sample.tangent.z
                                    }
                                },
                                {
                                    "half_width_meters",
                                    sample.
                                        halfWidthMeters
                                },
                                {
                                    "lanes",
                                    static_cast<
                                        orbit::i64>(
                                            sample.lanes)
                                }
                            });
                    }

                    return orbit::rpc::Value(
                        orbit::rpc::Value::Object{
                            {
                                "edge",
                                edge.ToString()
                            },
                            {"ready", true},
                            {
                                "frame",
                                product.frame.
                                    ToString()
                            },
                            {
                                "build_signature",
                                static_cast<
                                    orbit::i64>(
                                        product.
                                            buildSignature &
                                        0x7fffffffffffffffULL)
                            },
                            {
                                "width_meters",
                                product.widthMeters
                            },
                            {
                                "lane_count",
                                static_cast<
                                    orbit::i64>(
                                        product.
                                            laneCount)
                            },
                            {
                                "stations",
                                static_cast<
                                    orbit::i64>(
                                        product.
                                            stations.
                                            size())
                            },
                            {
                                "visual_vertices",
                                static_cast<
                                    orbit::i64>(
                                        product.
                                            visualMesh.
                                            vertices.
                                            size())
                            },
                            {
                                "visual_indices",
                                static_cast<
                                    orbit::i64>(
                                        product.
                                            visualMesh.
                                            indices.
                                            size())
                            },
                            {
                                "collision_triangles",
                                static_cast<
                                    orbit::i64>(
                                        product.
                                            collision.
                                            size())
                            },
                            {
                                "reference_nodes",
                                static_cast<
                                    orbit::i64>(
                                        product.
                                            referenceGraph.
                                            nodes.
                                            size())
                            },
                            {
                                "reference_edges",
                                static_cast<
                                    orbit::i64>(
                                        product.
                                            referenceGraph.
                                            edges.
                                            size())
                            },
                            {
                                "lanes",
                                std::move(lanes)
                            },
                            {
                                "navigation",
                                std::move(
                                    navigation)
                            }
                        });
                }
        });

        if (worldSession.HasWorld())
        {
            requestRoutedPaths();
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

        orbit::f64 viewportNavigationSpeedScale = 1.0;
        orbit::f64 viewportFrameDeltaSeconds = 0.0;
        orbit::platform::MouseDelta viewportFrameMouseDelta{};
        bool viewportRightGestureActive = false;
        bool viewportRightGestureDragged = false;
        orbit::i32 viewportRightGestureDistance = 0;
        bool viewportHomeWasDown = false;
        bool viewportEndWasDown = false;

        ui.RegisterPanel({
            .id = kViewportPanel,
            .title = "Viewport",
            .defaultOpen = true,
            .draw =
                [&studioViews,
                 &selection,
                 &bodyObject,
                 &objects,
                 &content,
                 &authoringCommands,
                 &presentActions,
                 &commandService,
                 &pathPlacementMode,
                 &pathDebugVisualization,
                 &activePathNetwork,
                 &lastPlacedPathNode,
                 &bodies,
                 &bodyId,
                 &studioSession,
                 &worldSession,
                 &window,
                 &viewportNavigationSpeedScale,
                 &viewportFrameDeltaSeconds,
                 &viewportFrameMouseDelta,
                 &viewportRightGestureActive,
                 &viewportRightGestureDragged,
                 &viewportRightGestureDistance,
                 &viewportHomeWasDown,
                 &viewportEndWasDown](
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
                            primaryView->Width() ||
                        height !=
                            primaryView->Height())
                    {
                        primaryView->Resize(
                            width,
                            height);
                    }

                    const auto interaction =
                        context.Image(
                            primaryView->Color(),
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

                    if (interaction.doubleClicked &&
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
                 &presentActions,
                 &worldSession](
                    orbit::editor_ui::
                        PanelContext& context)
                {
                    if (!worldSession.HasWorld())
                    {
                        context.Text("No world is open.");
                        return;
                    }

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

        ui.RegisterPanel({
            .id = kPropertiesPanel,
            .title = "Properties",
            .defaultOpen = true,
            .draw =
                [&inspector,
                 &presentActions,
                 &content,
                 &worldSession](
                    orbit::editor_ui::
                        PanelContext& context)
                {
                    if (!worldSession.HasWorld())
                    {
                        context.Text("No world is open.");
                        return;
                    }

                    const auto selected =
                        inspector().SelectedObjects();

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
                         inspector().CommonProperties())
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
                                inspector().
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
                 &pluginValidationIssues,
                 &worldSession](
                    orbit::editor_ui::
                        PanelContext& context)
                {
                    if (!worldSession.HasWorld())
                    {
                        context.Text("No world is open.");
                        return;
                    }

                    const auto statuses =
                        plugins().Statuses();

                    if (context.Button(
                            "Validate"))
                    {
                        pluginValidationIssues =
                            plugins().Validate();
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
                            if (!plugins().Reload(
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
                 &materialView,
                 &materialPreviewAsset,
                 &materialPreviewMaterial,
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
                        materialView.Resize(
                            previewPixelsWide,
                            previewPixelsHigh);
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

        ui.RegisterPanel({
            .id = kPlatformServicesPanel,
            .title = "Platform Services",
            .defaultOpen = false,
            .draw =
                [&project,
                 &platformConfiguration,
                 &platformConfigurationIssues,
                 &platformConfigurationPath,
                 &platformStatus,
                 &steamAppIdEditor,
                 &newAchievementId,
                 &newAchievementApiName,
                 &newStatId,
                 &newStatApiName,
                 &newStatFloat,
                 &newTimelineEventId,
                 &newTimelineTitle,
                 &newTimelineDescription,
                 &newTimelineIcon](
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

        ui.RegisterPanel({
            .id = kBuildPanel,
            .title = "Build",
            .defaultOpen = true,
            .draw =
                [&project,
                 &selectedBuildProfile,
                 &buildIssues,
                 &lastBuildManifest,
                 &lastPackageExecutable,
                 &buildStatus,
                 &validateProjectBuild,
                 &cookProjectBuild,
                 &packageProjectBuild](
                    orbit::editor_ui::
                        PanelContext& context)
                {
                    context.Text(
                        project.Manifest().
                            displayName);
                    context.Separator();

                    const auto& profiles =
                        project.Manifest().
                            buildProfiles;

                    if (profiles.empty())
                    {
                        context.Text(
                            "No build profiles are defined.");
                    }
                    else
                    {
                        context.Text(
                            "Build profile");

                        for (std::size_t index = 0;
                             index < profiles.size();
                             ++index)
                        {
                            const auto& profile =
                                profiles[index];

                            const bool selected =
                                profile.name ==
                                selectedBuildProfile;

                            const std::string button =
                                std::format(
                                    "{}##build-profile-{}",
                                    selected
                                        ? "Selected"
                                        : "Use",
                                    index);

                            if (context.Button(
                                    button))
                            {
                                selectedBuildProfile =
                                    profile.name;
                            }

                            context.SameLine();
                            context.Text(
                                std::format(
                                    "{} — {} / {} / {}",
                                    profile.name,
                                    profile.platform,
                                    profile.configuration,
                                    profile.storefront));
                        }
                    }

                    context.Separator();

                    if (context.Button(
                            "Validate Project"))
                    {
                        validateProjectBuild();
                    }

                    context.SameLine();

                    if (context.Button(
                            "Cook Project"))
                    {
                        cookProjectBuild();
                    }

                    context.SameLine();

                    if (context.Button(
                            "Package Project"))
                    {
                        static_cast<void>(
                            packageProjectBuild(
                                selectedBuildProfile));
                    }

                    context.Separator();
                    context.Text(
                        "Status: " +
                        buildStatus);

                    if (!lastBuildManifest.empty())
                    {
                        context.Text(
                            "Manifest: " +
                            lastBuildManifest.
                                generic_string());
                    }

                    if (!lastPackageExecutable.empty())
                    {
                        context.Text(
                            "Executable: " +
                            lastPackageExecutable.
                                generic_string());
                    }

                    for (const auto& issue :
                         buildIssues)
                    {
                        context.Text(
                            std::format(
                                "{} [{}] {}{}",
                                issue.severity ==
                                        orbit::build::
                                            IssueSeverity::Error
                                    ? "ERROR"
                                    : "WARN",
                                issue.code,
                                issue.message,
                                issue.path.empty()
                                    ? std::string{}
                                    : std::format(
                                          " ({})",
                                          issue.path.
                                              generic_string())));
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
                [&validateProjectBuild]
                {
                    validateProjectBuild();
                }
        });

        ui.RegisterMenuAction({
            .menu = "Build",
            .label = "Cook Project",
            .invoke =
                [&cookProjectBuild]
                {
                    cookProjectBuild();
                }
        });

        ui.RegisterMenuAction({
            .menu = "Build",
            .label = "Package Project",
            .invoke =
                [&packageProjectBuild,
                 &selectedBuildProfile]
                {
                    static_cast<void>(
                        packageProjectBuild(
                            selectedBuildProfile));
                }
        });

        ui.RegisterMenuAction({
            .menu = "Home",
            .label = "Undo",
            .invoke =
                [&authoringCommands, &worldSession]
                {
                    if (!worldSession.HasWorld())
                    {
                        return;
                    }
                    authoringCommands().Invoke(
                        orbit::editor_model::
                            authoring_commands::
                                kUndo);
                },
            .enabled =
                [&authoringCommands, &worldSession]
                {
                    if (!worldSession.HasWorld())
                    {
                        return false;
                    }
                    return authoringCommands().
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
                [&authoringCommands, &worldSession]
                {
                    if (!worldSession.HasWorld())
                    {
                        return;
                    }
                    authoringCommands().Invoke(
                        orbit::editor_model::
                            authoring_commands::
                                kRedo);
                },
            .enabled =
                [&authoringCommands, &worldSession]
                {
                    if (!worldSession.HasWorld())
                    {
                        return false;
                    }
                    return authoringCommands().
                        Enablement(
                            orbit::editor_model::
                                authoring_commands::
                                    kRedo).
                        enabled;
                }
        });

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

        const auto synchronizeActiveBodyPreview =
            [&]
            {
                const auto& activeBody =
                    studioSession.ActiveBody().Active();

                if (activeBody.has_value())
                {
                    const bool activeIdentityChanged =
                        bodyObject !=
                            activeBody->semanticObject ||
                        bodyId != activeBody->body;

                    bodyObject =
                        activeBody->semanticObject;
                    bodyId =
                        activeBody->body;

                    bodyView.Camera().frame =
                        activeBody->frame;

                    const orbit::f64 activeRadius =
                        std::max(
                            activeBody->
                                referenceRadiusMeters,
                            1.0);

                    bodyView.Camera().
                        nearPlaneMeters =
                            static_cast<orbit::f32>(
                                std::max(
                                    activeRadius *
                                        1.0e-6,
                                    1.0));
                    bodyView.Camera().
                        farPlaneMeters =
                            static_cast<orbit::f32>(
                                activeRadius * 10.0);

                    if (activeIdentityChanged)
                    {
                        bodyView.Camera().
                            localPositionMeters = {
                                0.0,
                                0.0,
                                -activeRadius * 3.2
                            };
                    }
                }
                else
                {
                    bodyObject = {};
                    bodyId = {};
                }
            };

        while (window.PumpEvents())
        {
            if (submittedFence != 0)
            {
                fence->Wait(
                    submittedFence);
            }

            const auto frameCpuStarted =
                Clock::now();

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

            const auto studioTick =
                studioSession.Tick(false);

            synchronizeActiveBodyPreview();

            if (studioTick.pathRoutingRebound)
            {
                routedObjectRevision =
                    ~orbit::u64{0};
                publishedRouteGeneration.
                    clear();
            }

            if (studioTick.pathProductsInvalidated)
            {
                derivedObjectRevision =
                    ~orbit::u64{0};
                derivedPaths.clear();
                derivedRouteGeneration.clear();
            }

            if (worldSession.HasWorld())
            {
                pollRoutedPaths();
                refreshDerivedPaths();
            }
            else
            {
                derivedPaths.clear();
                knownRoutedEdges.clear();
                activePathNetwork.reset();
                lastPlacedPathNode.reset();
                pathPlacementMode = false;
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

            viewportFrameDeltaSeconds =
                deltaSeconds;
            viewportFrameMouseDelta =
                window.ConsumeMouseDelta();

            ui.BeginFrame(
                window,
                deltaSeconds);

            ui.DrawStudioShell();

            if (viewportRightGestureActive &&
                !window.MouseButtonDown(
                    orbit::platform::
                        MouseButton::Right))
            {
                if (window.RelativeMouseMode())
                {
                    window.SetRelativeMouseMode(
                        false);
                }

                viewportRightGestureActive =
                    false;
                viewportRightGestureDragged =
                    false;
                viewportRightGestureDistance =
                    0;
            }

            synchronizeActiveBodyPreview();

            if (!worldSession.HasWorld())
            {
                derivedPaths.clear();
                knownRoutedEdges.clear();
                activePathNetwork.reset();
                lastPlacedPathNode.reset();
                pathPlacementMode = false;
            }

            if (worldSession.HasWorld())
            {
                shortcuts.Update(
                    window,
                    authoringCommands(),
                    ui.WantsKeyboard());
            }

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

            const auto* previewBody =
                bodyId.IsValid()
                    ? bodies().FindBody(bodyId)
                    : nullptr;

            std::vector<
                const orbit::path_geometry::
                    PathDerivedProduct*>
                visiblePathProducts;

            visiblePathProducts.reserve(
                derivedPaths.size());

            for (const auto&
                     [edge, product] :
                 derivedPaths)
            {
                static_cast<void>(edge);
                visiblePathProducts.push_back(
                    &product);
            }

            std::sort(
                visiblePathProducts.begin(),
                visiblePathProducts.end(),
                [](const auto* a,
                   const auto* b)
                {
                    if (a->edge.high !=
                        b->edge.high)
                    {
                        return a->edge.high <
                            b->edge.high;
                    }

                    return a->edge.low <
                        b->edge.low;
                });

            if (previewBody != nullptr)
            {
                const orbit::universe::BodyShape
                    previewShape =
                        previewBody->shape;

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
                    [&, previewShape](
                        orbit::rhi::CommandList&
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
            }
            else
            {
                graph.AddPass(
                    "Studio.BodyPreview.Blank",
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
                    [&bodyView](
                        orbit::rhi::CommandList&
                            commandList,
                        const orbit::render_graph::
                            Resources&)
                    {
                        commandList.ClearColorTarget(
                            bodyView.Color(),
                            {
                                .red = 0.018F,
                                .green = 0.021F,
                                .blue = 0.027F,
                                .alpha = 1.0F
                            });
                    });
            }

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
                        materialPreviewMaterial);
                });

            if (previewBody != nullptr)
            {
                graph.AddPass(
                "Studio.Paths",
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
                    pathPreview.Draw(
                        commandList,
                        bodyView.Color(),
                        bodyView.Width(),
                        bodyView.Height(),
                        bodyView.Camera(),
                        frames(),
                        {},
                        std::span<
                            const orbit::
                                path_geometry::
                                    PathDerivedProduct*
                                    const>(
                            visiblePathProducts.
                                data(),
                            visiblePathProducts.
                                size()),
                        pathDebugVisualization);
                });

            }

            const auto studioSnapshot =
                studioRuntime.Capture();

            const auto renderedStudioViews =
                studioViewportRenderer.Compose(
                    graph,
                    studioViews,
                    studioSession,
                    studioRuntime,
                    studioSnapshot,
                    {},
                    pathDebugVisualization,
                    swapchain.
                        CurrentBackBufferIndex());

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
                        renderedView.targets.color,
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

            graph.Execute(*commands);

            commands->Close();

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
