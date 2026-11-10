#include "EditorAppSupport.hpp"

#include <orbit/content/RuntimeTexture.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/editor_model/BuiltinSchemas.hpp>
#include <orbit/lighting/MaterialEmission.hpp>
#include <orbit/studio_session/ProjectBrowserModel.hpp>
#include <orbit/studio_session/StudioWorkspace.hpp>
#include <orbit/platform/Paths.hpp>
#include <orbit/universe/BodyRegistry.hpp>
#include <orbit/universe/ReferenceSurface.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <cstring>
#include <format>
#include <stdexcept>
#include <utility>

namespace orbit::editor_app::support
{
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

// Studio always opens straight into the editor. With no explicit project the
// most recently used one is reopened; on a first run a starter project is
// created. Switching/creating projects happens from the in-editor Project
// Browser panel.
[[nodiscard]] std::optional<
    orbit::documents::ProjectDocument>
OpenDefaultProject()
{
    const auto dataDirectory =
        orbit::platform::UserDataDirectory();

    {
        orbit::studio_session::StudioWorkspace
            scratch;
        const orbit::studio_session::
            ProjectBrowserModel browser(
                scratch,
                dataDirectory /
                    "RecentProjects.txt");

        for (const auto& item :
             browser.RecentProjects())
        {
            if (item.available)
            {
                return orbit::documents::
                    ProjectDocument::Open(
                        item.manifestPath);
            }
        }
    }

    const auto root =
        dataDirectory /
        "Projects" /
        "Untitled Project";

    if (std::filesystem::exists(
            root / "Project.orbit.toml"))
    {
        return orbit::documents::
            ProjectDocument::Open(
                root / "Project.orbit.toml");
    }

    return orbit::documents::
        ProjectDocument::Create(
            root,
            "Untitled Project");
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
} // namespace

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

    return OpenDefaultProject();
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

[[nodiscard]] orbit::editor_ui::PreviewMaterial
PreviewMaterialForAsset(
    orbit::content::ContentService& content,
    const orbit::content::AssetRecord* asset,
    const orbit::u32 depth)
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

        const auto emission =
            content.ResolveMaterialEmission(
                asset->id);

        const auto evaluated =
            orbit::lighting::
                EvaluateMaterialEmission({
                    .colorLinear = {
                        static_cast<orbit::f32>(
                            emission.colorLinear[0]),
                        static_cast<orbit::f32>(
                            emission.colorLinear[1]),
                        static_cast<orbit::f32>(
                            emission.colorLinear[2])
                    },
                    .luminanceNits =
                        static_cast<orbit::f32>(
                            emission.luminanceNits),
                    .contributesToGi =
                        emission.contributesToGi,
                    .giScale =
                        static_cast<orbit::f32>(
                            emission.giScale)
                });

        result.emissionRadiance =
            evaluated.visibleRadiance;

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

        const auto emission =
            content.ResolveMaterialEmission(
                asset->id);

        const auto evaluated =
            orbit::lighting::
                EvaluateMaterialEmission({
                    .colorLinear = {
                        static_cast<orbit::f32>(
                            emission.colorLinear[0]),
                        static_cast<orbit::f32>(
                            emission.colorLinear[1]),
                        static_cast<orbit::f32>(
                            emission.colorLinear[2])
                    },
                    .luminanceNits =
                        static_cast<orbit::f32>(
                            emission.luminanceNits),
                    .contributesToGi =
                        emission.contributesToGi,
                    .giScale =
                        static_cast<orbit::f32>(
                            emission.giScale)
                });

        result.emissionRadiance =
            evaluated.visibleRadiance;

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

// Turns one automation (MCP/RPC) exchange into user-facing toasts. Mutating
// commands are always announced; read-only queries only surface when they
// fail, so polling clients do not flood the screen.
void RecordRpcNotifications(
    const orbit::rpc::Dispatcher& dispatcher,
    const std::string_view message,
    const std::optional<std::string>& response,
    std::vector<orbit::editor_ui::EditorUi::Notification>& out)
{
    using orbit::editor_ui::EditorUi;

    try
    {
        const orbit::rpc::Value request =
            orbit::rpc::ParseValue(message);

        std::optional<orbit::rpc::Value> reply;
        if (response.has_value())
        {
            reply = orbit::rpc::ParseValue(*response);
        }

        const auto describeOne =
            [&](const orbit::rpc::Value& call,
                const orbit::rpc::Value* result)
        {
            if (!call.IsObject())
            {
                return;
            }

            const auto* method = call.Find("method");
            if (method == nullptr || !method->IsString())
            {
                return;
            }

            const std::string& name = method->AsString();
            const auto* error =
                result != nullptr && result->IsObject()
                    ? result->Find("error")
                    : nullptr;

            if (error != nullptr)
            {
                const auto* text =
                    error->IsObject()
                        ? error->Find("message")
                        : nullptr;
                out.push_back({
                    .title = "MCP command failed: " + name,
                    .detail =
                        text != nullptr && text->IsString()
                            ? text->AsString()
                            : std::string{},
                    .severity =
                        EditorUi::NotificationSeverity::Error,
                    .seconds = 6.0F});
                return;
            }

            bool mutating = false;
            for (const auto& descriptor : dispatcher.Catalog())
            {
                if (descriptor.name == name)
                {
                    mutating = descriptor.mutating;
                    break;
                }
            }

            if (mutating)
            {
                out.push_back({
                    .title = "MCP: " + name});
            }
        };

        if (request.IsArray())
        {
            // Batch responses are not guaranteed to be in request order, so
            // batch entries are announced without per-call error matching.
            for (const auto& call : request.AsArray())
            {
                describeOne(call, nullptr);
            }
        }
        else
        {
            describeOne(
                request,
                reply.has_value() ? &*reply : nullptr);
        }
    }
    catch (...)
    {
        // Malformed traffic gets its JSON-RPC error from the dispatcher; the
        // toast layer must never affect the response.
    }
}
} // namespace orbit::editor_app::support
