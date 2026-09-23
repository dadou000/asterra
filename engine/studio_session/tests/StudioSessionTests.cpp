#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/studio_session/VolumeSurfaceOutputResolver.hpp>
#include <orbit/volume_representation/VolumeCache.hpp>
#include <orbit/volume_representation/VolumeOutputCoupling.hpp>
#include <orbit/volume_representation/VolumeOutputRuntime.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr << "Studio session test failed.\n";
        std::exit(1);
    }
}

[[nodiscard]] orbit::rpc::Value RpcCall(
    orbit::studio_session::StudioSession& studio,
    const std::string& id,
    const std::string& method,
    orbit::rpc::Value params =
        orbit::rpc::Value(
            orbit::rpc::Value::Object{}))
{
    const orbit::rpc::Value request(
        orbit::rpc::Value::Object{
            {"jsonrpc", "2.0"},
            {"id", id},
            {"method", method},
            {"params", std::move(params)}
        });

    const auto responseText =
        studio.DispatchRpc(
            orbit::rpc::Serialize(request));
    Check(responseText.has_value());

    const auto response =
        orbit::rpc::ParseValue(*responseText);
    Check(response.Find("error") == nullptr);
    Check(response.Find("result") != nullptr);
    return *response.Find("result");
}

orbit::scene::ObjectId AddBody(
    orbit::studio_session::StudioSession& studio,
    const std::string& name,
    const double radius)
{
    auto& world = studio.World();
    auto roots = world.Objects().Roots();

    orbit::scene::ObjectId worldObject{};
    if (roots.empty())
    {
        worldObject =
            world.Commands().CreateObject(
                orbit::world_model::kWorldType,
                "World");
    }
    else
    {
        worldObject = roots.front().id;
    }

    const auto system =
        world.Commands().CreateObject(
            orbit::world_model::kCelestialSystemType,
            "Helion",
            worldObject);
    const auto body =
        world.Commands().CreateObject(
            orbit::world_model::kCelestialBodyType,
            name,
            system);

    world.Commands().SetProperty(
        body,
        orbit::world_model::kBodyRadius,
        radius);
    world.Commands().SetProperty(
        body,
        orbit::world_model::kBodyMass,
        5.0e24);
    return body;
}

void CheckVolumeOutputSimulationCadence(
    orbit::studio_session::StudioSession& studio)
{
    using namespace orbit;
    using namespace orbit::volume_representation;

    auto roots = studio.World().Objects().Roots();
    Check(!roots.empty());

    auto& commands = studio.World().Commands();
    const auto volume = commands.CreateObject(
        world_model::kVolumeType,
        "M38 Tick Volume",
        roots.front().id);

    commands.SetProperty(
        volume,
        world_model::kVolumeHalfExtentsMeters,
        math::Double3{2.0, 2.0, 2.0});
    commands.SetProperty(
        volume,
        world_model::kVolumeFieldMask,
        static_cast<i64>(world_model::VolumeField::Density));

    const auto source = commands.CreateObject(
        world_model::kVolumeSourceType,
        "M38 Density Source",
        volume);
    commands.SetProperty(
        source,
        world_model::kVolumeChildShape,
        static_cast<i64>(world_model::VolumeSourceShape::Sphere));
    commands.SetProperty(
        source,
        world_model::kVolumeChildRadiusMeters,
        10.0);
    commands.SetProperty(
        source,
        world_model::kVolumeChildScalar,
        1.0);
    commands.SetProperty(
        source,
        world_model::kVolumeChildFieldMask,
        static_cast<i64>(world_model::VolumeField::Density));

    const auto domain =
        world_model::ResolveVolumeDomain(
            studio.World().Objects(),
            volume);
    Check(domain.has_value());

    const auto inputs =
        world_model::ResolveVolumeInputs(
            studio.World().Objects(),
            volume);
    Check(inputs.size() == 1U);

    const VolumeCacheBakeSettings bakeSettings{
        .resolution = 4U,
        .fieldMask = static_cast<u64>(
            world_model::VolumeField::Density)
    };
    auto cache = BakeVolumeCache(
        *domain,
        inputs,
        bakeSettings);
    Check(!cache.density.empty());
    VolumeCaches().Attach(volume, std::move(cache));

    auto& settings = VolumeOutputs().Settings(volume);
    settings.particlesEnabled = true;
    settings.surfaceDepositsEnabled = false;
    settings.fieldThreshold = 0.1F;
    settings.particleRatePerSecond = 10.0F;
    settings.particleBudgetPerStep = 10U;

    // StudioSession::Tick is the sole M38 producer cadence. Advancing the
    // simulation clock by half a second produces exactly five requests.
    studio.Clock().Advance(0.5);
    static_cast<void>(studio.Tick(false));
    Check(VolumeParticleRequests().Pending().size() == 5U);
    Check(
        VolumeOutputRuntimeService().Diagnostics().
            dispatchedParticleRequests == 5U);

    // A second Studio tick without simulation-time advance starts a fresh
    // step, clears the previous queue, and emits no duplicate render/UI work.
    static_cast<void>(studio.Tick(false));
    Check(VolumeParticleRequests().Pending().empty());
    Check(
        VolumeOutputRuntimeService().Diagnostics().
            dispatchedParticleRequests == 0U);

    VolumeCaches().Detach(volume);
    VolumeOutputs().Reset(volume);
}

void CheckVolumeSurfaceProjection(
    orbit::studio_session::StudioSession& studio,
    const orbit::scene::ObjectId semanticBody,
    const orbit::universe::BodyId runtimeBody,
    const orbit::f64 radiusMeters)
{
    using namespace orbit;
    using namespace orbit::studio_session;
    using namespace orbit::volume_representation;

    const auto volume =
        studio.World().Commands().CreateObject(
            world_model::kVolumeType,
            "M38 Surface Receiver Volume",
            semanticBody);

    const VolumeSurfaceQueuedRequest request{
        .volume = volume,
        .request = {
            .eventId = 0x380001U,
            .samplePositionMeters = {
                radiusMeters + 10.0,
                0.0,
                0.0
            },
            .maximumProjectionDistanceMeters = 20.0,
            .radiusMeters = 0.5F,
            .amount = 0.75F,
            .density = 1.0F,
            .emission = 0.0F
        }
    };

    const VolumeSurfaceQueuedRequest requests[]{request};

    auto& resolver = VolumeSurfaceOutputs();
    resolver.Resolve(
        studio.World().Objects(),
        studio.World().Universe(),
        studio.World().Surfaces(),
        requests);

    Check(resolver.Diagnostics().submitted == 1U);
    Check(resolver.Diagnostics().resolved == 1U);
    Check(resolver.Resolved().size() == 1U);

    const auto& resolved = resolver.Resolved().front();
    Check(resolved.volume == volume);
    Check(resolved.body == runtimeBody);
    Check(resolved.request.eventId == 0x380001U);
    Check(std::abs(
        resolved.bodyLocalSurfacePointMeters.x -
        radiusMeters) < 1.0e-4);
    Check(std::abs(resolved.bodyLocalSurfacePointMeters.y) < 1.0e-6);
    Check(std::abs(resolved.bodyLocalSurfacePointMeters.z) < 1.0e-6);

    // This +X test is intentionally incompatible with a hidden global -Y
    // projection. The resolved point must stay on the +X radial line.
    Check(std::abs(resolved.coordinate.latitudeRadians) < 1.0e-8);
    Check(std::abs(resolved.coordinate.longitudeRadians) < 1.0e-8);

    auto outOfRange = request;
    outOfRange.request.maximumProjectionDistanceMeters = 5.0;
    const VolumeSurfaceQueuedRequest rejected[]{outOfRange};
    resolver.Resolve(
        studio.World().Objects(),
        studio.World().Universe(),
        studio.World().Surfaces(),
        rejected);
    Check(resolver.Resolved().empty());
    Check(resolver.Diagnostics().projectionOutOfRange == 1U);
}
} // namespace

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-studio-session-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Studio Session Test");
        orbit::studio_session::StudioSession studio(project);

        const auto initialWorldGeneration =
            studio.World().Generation();
        const auto initialUniverseGeneration =
            studio.World().UniverseGeneration();
        Check(initialWorldGeneration != 0);
        Check(initialUniverseGeneration != 0);

        const auto idleTick = studio.Tick();
        Check(!idleTick.compositionChanged);
        Check(
            idleTick.worldGeneration ==
            initialWorldGeneration);
        Check(
            idleTick.universeGeneration ==
            initialUniverseGeneration);

        const auto asterra =
            AddBody(
                studio,
                "Asterra",
                6'000'000.0);
        const auto composedTick = studio.Tick();
        Check(composedTick.compositionChanged);
        Check(
            composedTick.universeGeneration >
            initialUniverseGeneration);
        Check(studio.ActiveBody().Active().has_value());
        Check(
            studio.ActiveBody().Active()->semanticObject ==
            asterra);

        CheckVolumeOutputSimulationCadence(studio);

        const auto stableUniverseGeneration =
            studio.World().UniverseGeneration();
        const auto noOpTick = studio.Tick();
        Check(!noOpTick.compositionChanged);
        Check(
            noOpTick.universeGeneration ==
            stableUniverseGeneration);
        Check(
            studio.World().UniverseGeneration() ==
            stableUniverseGeneration);

        static_cast<void>(
            studio.World().RebuildUniverse());
        Check(
            studio.World().UniverseGeneration() >
            stableUniverseGeneration);

        const auto secondary =
            studio.CreateWorld(
                "Secondary",
                "Secondary");
        Check(studio.Worlds().size() == 2U);

        const auto beforeWorldSwitchUniverseGeneration =
            studio.World().UniverseGeneration();
        studio.OpenWorld(secondary.relativePath);
        Check(
            studio.World().UniverseGeneration() >
            beforeWorldSwitchUniverseGeneration);
        Check(studio.ActiveWorld().has_value());
        Check(
            studio.ActiveWorld()->descriptor.id ==
            secondary.id);
        Check(studio.World().Explorer().Roots().empty());
        Check(!studio.ActiveBody().Active().has_value());

        const auto secondaryEmptyGeneration =
            studio.World().UniverseGeneration();

        const auto secondaryWorldObject =
            studio.World().Commands().
                CreateObject(
                    orbit::world_model::kWorldType,
                    "World");

        const orbit::scene::ObjectId
            selectedWorld[] = {
                secondaryWorldObject
            };

        studio.World().Selection().Set(
            selectedWorld);

        studio.World().CommandRegistry().
            Invoke(
                orbit::editor_model::
                    authoring_commands::
                        kCreateRockyPlanet,
                {
                    {
                        "name",
                        std::string("Veyra")
                    },
                    {
                        "radiusMeters",
                        4'200'000.0
                    }
                });

        Check(
            studio.World().Selection().
                Ordered().size() ==
            1U);

        const auto veyra =
            studio.World().Selection().
                Ordered().front();

        const auto secondaryComposedTick =
            studio.Tick();

        Check(
            secondaryComposedTick.
                compositionChanged);
        Check(
            secondaryComposedTick.universeGeneration >
            secondaryEmptyGeneration);
        Check(studio.ActiveBody().Active().has_value());
        Check(
            studio.ActiveBody().Active()->semanticObject ==
            veyra);
        Check(
            studio.World().SurfaceStats().
                terrainSurfaces ==
            1U);

        const auto activeRuntimeBody =
            studio.ActiveBody().Active()->
                body;

        const auto* terrainServices =
            studio.World().Surfaces().
                ServicesForBody(
                    activeRuntimeBody);

        Check(
            terrainServices !=
                nullptr);
        Check(
            terrainServices->
                IsValid());
        Check(
            terrainServices->
                Biomes().
                Definitions().
                size() ==
            1U);
        Check(
            terrainServices->
                Biomes().
                BaseBiome().
                IsValid());

        const auto veyraChildren =
            studio.World().Objects().
                Children(
                    veyra);

        Check(
            veyraChildren.size() ==
                1U &&
            veyraChildren.front().type ==
                orbit::world_model::
                    kTerrainSurfaceType);

        CheckVolumeSurfaceProjection(
            studio,
            veyra,
            activeRuntimeBody,
            4'200'000.0);

        const auto roots =
            RpcCall(
                studio,
                "1",
                "object.roots");
        Check(roots.IsArray());
        Check(roots.AsArray().size() == 1U);
        Check(
            roots.AsArray().front().
                Find("name")->AsString() ==
            "World");

        const auto beforeCloseUniverseGeneration =
            studio.World().UniverseGeneration();
        studio.CloseWorld();
        Check(!studio.World().HasWorld());
        Check(
            studio.World().UniverseGeneration() >
            beforeCloseUniverseGeneration);
        Check(!studio.ActiveWorld().has_value());
        Check(!studio.ActiveBody().Active().has_value());

        const auto catalog =
            RpcCall(
                studio,
                "2",
                "world.list");
        Check(catalog.IsArray());
        Check(catalog.AsArray().size() == 2U);

        const auto beforeReopenUniverseGeneration =
            studio.World().UniverseGeneration();
        studio.OpenWorld("Main");
        const auto reopenedTick = studio.Tick();
        Check(
            reopenedTick.universeGeneration >
            beforeReopenUniverseGeneration);
        Check(studio.World().HasWorld());
        Check(studio.ActiveBody().Active().has_value());
        Check(
            studio.ActiveBody().Active()->semanticObject ==
            asterra);
        Check(
            studio.World().Explorer().Roots().size() ==
            1U);
    }

    std::filesystem::remove_all(root);
    return 0;
}
