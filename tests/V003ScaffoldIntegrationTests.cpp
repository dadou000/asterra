#include <orbit/build/BuildService.hpp>
#include <orbit/commands/CommandRegistry.hpp>
#include <orbit/commands/CommandService.hpp>
#include <orbit/content/ContentService.hpp>
#include <orbit/content_wic/WicTextureImporter.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/BuiltinSchemas.hpp>
#include <orbit/editor_model/CommandSurfaces.hpp>
#include <orbit/editor_rpc/EditorRpcService.hpp>
#include <orbit/frames/FrameGraph.hpp>
#include <orbit/paths/PathNetwork.hpp>
#include <orbit/platform_services/PlatformConfig.hpp>
#include <orbit/plugins/PluginManager.hpp>
#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/selection/SelectionService.hpp>
#include <orbit/surface/SurfaceRegistry.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/time/SimulationTime.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <source_location>
#include <span>
#include <string>
#include <string_view>

namespace
{
void Check(
    const bool condition,
    const std::source_location location =
        std::source_location::current())
{
    if (!condition)
    {
        std::cerr
            << "V0.0.3 integration proof failed at "
            << location.file_name()
            << ':'
            << location.line()
            << '\n';
        std::exit(1);
    }
}

void Stage(const std::string_view name)
{
    std::cerr
        << "[V0.0.3 integration] "
        << name
        << '\n'
        << std::flush;
}

void WriteText(
    const std::filesystem::path& path,
    const std::string_view text)
{
    std::filesystem::create_directories(
        path.parent_path());
    std::ofstream output(
        path,
        std::ios::binary |
            std::ios::trunc);
    Check(output.good());
    output << text;
    Check(output.good());
}

void WriteTinyBmp(
    const std::filesystem::path& path)
{
    constexpr std::array<unsigned char, 58> bytes{
        0x42, 0x4d,
        0x3a, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x36, 0x00, 0x00, 0x00,
        0x28, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x01, 0x00,
        0x20, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x04, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x30, 0x70, 0xb0, 0xff
    };

    std::filesystem::create_directories(
        path.parent_path());
    std::ofstream output(
        path,
        std::ios::binary |
            std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    Check(output.good());
}

[[nodiscard]] std::string ReadText(
    const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    Check(input.good());
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

[[nodiscard]] orbit::rpc::Value Call(
    orbit::rpc::Dispatcher& dispatcher,
    std::string id,
    std::string method,
    orbit::rpc::Value params =
        orbit::rpc::Value(
            orbit::rpc::Value::Object{}))
{
    orbit::rpc::Value request(
        orbit::rpc::Value::Object{
            {"jsonrpc", "2.0"},
            {"id", std::move(id)},
            {"method", std::move(method)},
            {"params", std::move(params)}
        });

    const auto response =
        dispatcher.Dispatch(
            orbit::rpc::Serialize(request));
    Check(response.has_value());

    const orbit::rpc::Value document =
        orbit::rpc::ParseValue(*response);
    Check(document.Find("error") == nullptr);
    const auto* result = document.Find("result");
    Check(result != nullptr);
    return *result;
}
} // namespace

int main(const int argc, char** argv)
{
    Stage("bootstrap");
    Check(argc >= 2);

    const std::filesystem::path playerExecutable =
        argv[1];
    Check(std::filesystem::is_regular_file(
        playerExecutable));

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-v003-proof-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    orbit::scene::ObjectId persistedBody{};
    orbit::scene::ObjectId persistedDecal{};
    std::string firstBuildManifest;

    {
        Stage("project-and-source-assets");
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "V0.0.3 Scaffold Proof");

        WriteTinyBmp(
            root / "Content" / "proof.bmp");
        WriteText(
            root / "Content" / "proof.orbitmaterial",
            "[material]\n"
            "name = \"Proof Material\"\n"
            "base_color = \"proof.bmp\"\n"
            "roughness_factor = 0.28\n"
            "metallic_factor = 0.72\n"
            "tags = [\"proof\", \"pbr\"]\n");
        WriteText(
            root / "Content" / "proof.orbitpathprofile",
            "[path_profile]\n"
            "name = \"Proof Road\"\n"
            "kind = \"road\"\n"
            "width_meters = 7.0\n"
            "lanes = 2\n"
            "minimum_radius_meters = 12.0\n"
            "maximum_grade = 0.25\n");
        WriteText(
            root / "Scripts" / "main.luau",
            "return { proof = true }\n");

        const auto pluginRoot =
            root / "Plugins" / "proof.plugin";
        WriteText(
            pluginRoot / "plugin.toml",
            "[plugin]\n"
            "id = \"proof.plugin\"\n"
            "version = \"1.0.0\"\n"
            "orbit_api = \"0.0.3\"\n"
            "entry = \"main.luau\"\n"
            "scope = \"editor\"\n"
            "permissions = []\n");
        WriteText(
            pluginRoot / "main.luau",
            "local cmd = Orbit.registerCommand('Proof Tool', function() end, 'Proof', 'V0.0.3 proof command', false)\n"
            "Orbit.registerContextAction(cmd, 'explorer', 'context')\n"
            "Orbit.registerPanel('Proof Panel', function() Orbit.ui.text('proof') end)\n"
            "Orbit.registerValidator(function() return nil end)\n");

        project.Manifest().scriptEntryPoints = {
            "Scripts/main.luau"
        };
        project.Manifest().plugins.push_back({
            .id = "proof.plugin",
            .version = "1.0.0"
        });
        project.Save();

        Stage("content-import");
        orbit::content::ContentService content(root);
        orbit::content_wic::RegisterTextureImporters(
            content.Importers());
        content.Scan();

        const auto materials =
            content.Search(
                "Proof Material",
                orbit::content::AssetKind::Material);
        Check(materials.size() == 1);
        Check(materials[0].derivedReady);

        const auto textures =
            content.Search(
                "proof",
                orbit::content::AssetKind::Texture);
        Check(textures.size() == 1);
        Check(textures[0].derivedReady);

        const auto decalAsset =
            content.CreateDecal(
                textures[0].id,
                "Proof Decal",
                4.0,
                2.0,
                0.85);
        Check(content.Find(decalAsset) != nullptr);

        Stage("semantic-authoring");
        orbit::documents::WorldDatabase world(
            project.StartupWorldPath());
        orbit::schema::SchemaRegistry schemas;
        orbit::editor_model::builtin::RegisterSchemas(
            schemas);

        orbit::scene::ObjectStore objects(world);
        orbit::selection::SelectionService selection;
        orbit::commands::CommandService commands(
            objects,
            schemas);
        orbit::commands::CommandRegistry commandRegistry;
        orbit::editor_model::authoring_commands::Register(
            commandRegistry,
            commands,
            objects,
            selection);
        orbit::editor_model::authoring_commands::
            RegisterMaterialCommands(
                commandRegistry,
                commands,
                objects,
                selection);

        const auto worldRoot =
            commands.CreateObject(
                orbit::editor_model::builtin::kWorldType,
                "World");
        const auto systemObject =
            commands.CreateObject(
                orbit::editor_model::builtin::kCelestialSystemType,
                "Helion",
                worldRoot);
        persistedBody =
            commands.CreateObject(
                orbit::editor_model::builtin::kCelestialBodyType,
                "Asterra",
                systemObject);
        commands.SetProperty(
            persistedBody,
            orbit::editor_model::builtin::kBodyRadius,
            6'000'000.0);

        const std::array bodySelection{
            persistedBody
        };
        selection.Set(std::span(bodySelection));

        commandRegistry.Invoke(
            orbit::editor_model::authoring_commands::kAssignMaterial,
            {
                {
                    "material",
                    materials[0].sourcePath.generic_string()
                }
            });
        Check(
            std::get<std::string>(
                *objects.GetProperty(
                    persistedBody,
                    orbit::editor_model::builtin::kBodyMaterialAsset)) ==
            materials[0].sourcePath.generic_string());

        const auto* decalRecord = content.Find(decalAsset);
        Check(decalRecord != nullptr);
        Check(decalRecord->decal.has_value());

        commandRegistry.Invoke(
            orbit::editor_model::authoring_commands::kAttachDecal,
            {
                {"decal", decalRecord->sourcePath.generic_string()},
                {"latitude", 0.15},
                {"longitude", -0.35},
                {"width", decalRecord->decal->widthMeters},
                {"height", decalRecord->decal->heightMeters},
                {"rotation", 12.0},
                {"opacity", decalRecord->decal->opacity}
            });

        Check(selection.Ordered().size() == 1);
        persistedDecal = selection.Ordered().front();
        Check(objects.Find(persistedDecal).has_value());
        Check(
            objects.Find(persistedDecal)->type ==
            orbit::editor_model::builtin::kSurfaceDecalType);

        commands.Undo();
        Check(!objects.Find(persistedDecal).has_value());
        commands.Redo();
        Check(objects.Find(persistedDecal).has_value());

        Stage("runtime-universe-and-terrain");
        orbit::frames::FrameGraph frameGraph;
        orbit::universe::BodyRegistry bodies(frameGraph);
        const auto system =
            bodies.CreateSystem("Proof System");
        const auto primary =
            bodies.CreateBody({
                .system = system,
                .name = "Asterra",
                .shape = orbit::universe::SphereShape{
                    .radiusMeters = 6'000'000.0
                },
                .mass = orbit::universe::MassProperties{
                    .massKilograms = 5.0e24
                }
            });
        const auto* primaryBody =
            bodies.FindBody(primary);
        Check(primaryBody != nullptr);

        // M23 must prove that the existing rocky-planet terrain stack is a
        // capability of a celestial body, not a parallel global planet.
        orbit::surface::SurfaceRegistry surfaces(bodies);
        auto terrainSource =
            std::make_shared<orbit::terrain::AnalyticTerrainSource>(
                orbit::world::PlanetDefinition{
                    .radiusMeters = 6'000'000.0
                });
        surfaces.AttachTerrain(primary, terrainSource);

        const auto* terrainCapability =
            surfaces.FindTerrainSurface(primary);
        Check(terrainCapability != nullptr);
        Check(terrainCapability->terrain == terrainSource);

        const auto terrainPlanet =
            surfaces.SphericalPlanetDefinition(primary);
        Check(terrainPlanet.has_value());
        Check(terrainPlanet->radiusMeters == 6'000'000.0);

        const orbit::terrain::TerrainSample terrainSample =
            terrainCapability->terrain->Sample({
                .unitDirection = {0.0, 1.0, 0.0},
                .footprintMeters = 100.0
            });
        Check(std::isfinite(terrainSample.elevationMeters));

        const auto movingBody =
            bodies.CreateBody({
                .system = system,
                .name = "Luma",
                .parentFrame = primaryBody->frame,
                .shape = orbit::universe::SphereShape{
                    .radiusMeters = 900'000.0
                },
                .transformModel =
                    orbit::universe::UniformRotationTransform{
                        .centerInParentMeters = {
                            12'000'000.0,
                            0.0,
                            0.0
                        },
                        .axisInParent = {0.0, 0.0, 1.0},
                        .angularVelocityRadiansPerSecond = 0.2,
                        .phaseRadiansAtEpoch = 0.0
                    }
            });
        const auto* moving =
            bodies.FindBody(movingBody);
        Check(moving != nullptr);
        Check(bodies.Bodies(system).size() == 2);
        const auto transform0 =
            frameGraph.ResolveTransform(
                moving->frame,
                primaryBody->frame,
                orbit::time::SimulationTime{});
        const auto transform1 =
            frameGraph.ResolveTransform(
                moving->frame,
                primaryBody->frame,
                orbit::time::SimulationTime{
                    .microsecondsFromEpoch = 1'000'000
                });
        Check(transform0.has_value());
        Check(transform1.has_value());
        Check(transform0->rotation != transform1->rotation);

        Stage("path-authoring");
        orbit::paths::PathNetworkService pathService(
            objects,
            commands);
        const auto profile =
            content.Search(
                "Proof Road",
                orbit::content::AssetKind::PathProfile);
        Check(profile.size() == 1);
        const auto network =
            pathService.CreateNetwork(
                "Proof Network",
                persistedBody,
                profile[0].id.ToString());
        const auto nodeA =
            pathService.CreateNode(
                network.id,
                "A",
                orbit::paths::SurfaceAnchor{
                    .body = primary,
                    .coordinate = {0.0, 0.0, 0.0}
                });
        const auto nodeB =
            pathService.CreateNode(
                network.id,
                "B",
                orbit::paths::SurfaceAnchor{
                    .body = primary,
                    .coordinate = {0.0, 0.001, 0.0}
                });
        const auto nodeC =
            pathService.CreateNode(
                network.id,
                "C",
                orbit::paths::SurfaceAnchor{
                    .body = primary,
                    .coordinate = {0.001, 0.002, 0.0}
                });
        const auto nodeD =
            pathService.CreateNode(
                network.id,
                "D",
                orbit::paths::SurfaceAnchor{
                    .body = primary,
                    .coordinate = {0.002, 0.003, 0.0}
                });
        const auto direct =
            pathService.ConnectDirect(nodeA.id, nodeB.id);
        const auto bezier =
            pathService.ConnectBezier(
                nodeB.id,
                nodeC.id,
                {25.0, 0.0, 0.0},
                {-25.0, 0.0, 0.0});
        const auto routed =
            pathService.ConnectRouted(nodeC.id, nodeD.id);
        Check(direct.mode == orbit::paths::EdgeMode::Direct);
        Check(bezier.mode == orbit::paths::EdgeMode::Bezier);
        Check(routed.mode == orbit::paths::EdgeMode::Routed);

        Stage("plugin-loading");
        orbit::editor_model::CommandSurfaceRegistry
            commandSurfaces;
        orbit::plugins::PluginManager plugins(
            root,
            commandRegistry,
            commands,
            commandSurfaces,
            objects,
            selection);
        plugins.LoadEnabled(project.Manifest());
        const auto statuses = plugins.Statuses();
        Check(statuses.size() == 1);
        Check(statuses[0].loaded);
        Check(plugins.PanelCatalog().size() == 1);

        Stage("rpc-automation");
        orbit::rpc::Dispatcher dispatcher;
        orbit::editor_rpc::EditorRpcService editorRpc(
            dispatcher,
            project,
            commandRegistry,
            commands,
            schemas,
            objects,
            selection);
        static_cast<void>(editorRpc);

        const auto rpcCreated =
            Call(
                dispatcher,
                "mcp-proof",
                "object.create",
                orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {
                            "type",
                            orbit::editor_model::builtin::
                                kCelestialBodyType.ToString()
                        },
                        {"name", "MCP Proof Body"},
                        {"parent", systemObject.ToString()}
                    }));
        Check(rpcCreated.Find("id") != nullptr);

        Stage("platform-configuration");
        orbit::platform_services::PlatformConfiguration
            platformConfig;
        platformConfig.steam.enabled = true;
        platformConfig.steam.appId = 480;
        platformConfig.steam.achievements.push_back({
            .id = "proof.complete",
            .steamApiName = "ACH_PROOF_COMPLETE"
        });
        platformConfig.steam.stats.push_back({
            .id = "proof.steps",
            .kind = orbit::platform_services::StatKind::Integer,
            .steamApiName = "STAT_PROOF_STEPS"
        });
        platformConfig.steam.timelineEvents.push_back({
            .eventId = "proof.completed",
            .title = "Scaffold proof completed",
            .description = "V0.0.3 integration proof",
            .icon = "steam_achievement",
            .priority = 10,
            .clipPriority =
                orbit::platform_services::TimelineClipPriority::Standard
        });
        orbit::platform_services::SavePlatformConfigurationAtomic(
            root / "Config" / "PlatformServices.toml",
            platformConfig);
        Check(
            orbit::platform_services::ValidatePlatformConfiguration(
                platformConfig,
                true).empty());

        project.Save();
        world.Checkpoint();

        std::filesystem::remove_all(
            root / ".orbit" / "DerivedData");

        orbit::build::BuildService buildService;
        const orbit::build::BuildRequest request{
            .manifestPath = project.ManifestPath(),
            .profileName = "Development Windows"
        };

        Stage("first-package");
        const auto firstPackage =
            buildService.Package(
                request,
                {
                    .playerExecutable = playerExecutable
                });
        if (!firstPackage.Succeeded())
        {
            for (const auto& issue : firstPackage.issues)
            {
                std::cerr
                    << "[V0.0.3 build issue] "
                    << issue.code
                    << ": "
                    << issue.message;
                if (!issue.path.empty())
                {
                    std::cerr
                        << " ["
                        << issue.path.generic_string()
                        << "]";
                }
                std::cerr << '\n';
            }
        }
        Check(firstPackage.Succeeded());
        Check(std::filesystem::is_regular_file(
            firstPackage.executablePath));
        Check(std::filesystem::is_regular_file(
            firstPackage.manifestPath));
        firstBuildManifest =
            ReadText(firstPackage.manifestPath);

        std::filesystem::remove_all(
            root / ".orbit" / "DerivedData");

        Stage("second-package");
        const auto secondPackage =
            buildService.Package(
                request,
                {
                    .playerExecutable = playerExecutable
                });
        if (!secondPackage.Succeeded())
        {
            for (const auto& issue : secondPackage.issues)
            {
                std::cerr
                    << "[V0.0.3 build issue] "
                    << issue.code
                    << ": "
                    << issue.message;
                if (!issue.path.empty())
                {
                    std::cerr
                        << " ["
                        << issue.path.generic_string()
                        << "]";
                }
                std::cerr << '\n';
            }
        }
        Check(secondPackage.Succeeded());
        Check(
            ReadText(secondPackage.manifestPath) ==
            firstBuildManifest);
    }

    {
        Stage("persistence-reopen");
        const auto reopenedProject =
            orbit::documents::ProjectDocument::Open(
                root / "Project.orbit.toml");
        orbit::documents::WorldDatabase reopenedWorld(
            reopenedProject.StartupWorldPath());
        orbit::scene::ObjectStore reopenedObjects(
            reopenedWorld);

        Check(reopenedObjects.Find(persistedBody).has_value());
        Check(reopenedObjects.Find(persistedDecal).has_value());
        Check(
            std::get<std::string>(
                *reopenedObjects.GetProperty(
                    persistedDecal,
                    orbit::editor_model::builtin::kDecalAsset)).
                find(".orbitdecal") !=
            std::string::npos);
    }

    Stage("complete");
    std::filesystem::remove_all(root);
    return 0;
}
