// The object store memoises its reads (Find / Roots / Children / GetProperty).
// Every kind of write, undo and rolled-back transaction must be visible to the
// next read.
#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <filesystem>
#include <variant>

namespace
{
using namespace orbit;
using namespace orbit::world_model;

bool Check(const bool condition, const int code, int& failure)
{
    if (!condition && failure == 0)
    {
        failure = code;
    }
    return condition;
}
} // namespace

int main()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-object-cache-" + documents::ProjectId::Random().ToString());
    std::filesystem::remove_all(root);

    int failure = 0;
    {
        auto project =
            documents::ProjectDocument::Create(root, "Object Cache Test");
        documents::WorldDatabase world(project.StartupWorldPath());
        schema::SchemaRegistry schemas;
        RegisterSchemas(schemas);
        scene::ObjectStore objects(world);
        commands::CommandService commands(objects, schemas);

        const auto worldObject = commands.CreateObject(kWorldType, "World");

        // Warm every cache, then mutate and read again.
        Check(objects.Children(worldObject).empty(), 1, failure);
        Check(objects.Roots().size() == 1U, 2, failure);

        const auto system =
            commands.CreateObject(kCelestialSystemType, "System", worldObject);
        Check(objects.Children(worldObject).size() == 1U, 3, failure);
        Check(objects.Find(system).has_value(), 4, failure);

        const auto body =
            commands.CreateObject(kCelestialBodyType, "Body", system);
        Check(objects.Children(system).size() == 1U, 5, failure);

        const auto atmo =
            commands.CreateObject(kAtmosphereCapabilityType, "Atmosphere", body);
        Check(objects.Children(body).size() == 1U, 22, failure);

        // Rename shows up in Find and in the parent's child list.
        Check(objects.Find(body)->name == "Body", 6, failure);
        commands.RenameObject(body, "Renamed");
        Check(objects.Find(body)->name == "Renamed", 7, failure);
        Check(objects.Children(system).front().name == "Renamed", 8, failure);

        // Properties, including a cached "not set" answer.
        Check(!objects.GetProperty(atmo, kAtmosphereRayleighScaleHeightMeters).has_value(), 9, failure);
        commands.SetProperty(atmo, kAtmosphereRayleighScaleHeightMeters, 1234.0);
        {
            const auto value = objects.GetProperty(atmo, kAtmosphereRayleighScaleHeightMeters);
            Check(value.has_value() && std::get<f64>(*value) == 1234.0, 10, failure);
        }
        commands.SetProperty(atmo, kAtmosphereRayleighScaleHeightMeters, 4321.0);
        {
            const auto value = objects.GetProperty(atmo, kAtmosphereRayleighScaleHeightMeters);
            Check(value.has_value() && std::get<f64>(*value) == 4321.0, 11, failure);
        }

        // Undo restores the previous reads.
        commands.Undo();
        {
            const auto value = objects.GetProperty(atmo, kAtmosphereRayleighScaleHeightMeters);
            Check(value.has_value() && std::get<f64>(*value) == 1234.0, 12, failure);
        }
        commands.Undo();
        Check(!objects.GetProperty(atmo, kAtmosphereRayleighScaleHeightMeters).has_value(), 13, failure);

        // Reparent moves the object between cached child lists.
        Check(objects.Children(worldObject).size() == 1U, 14, failure);
        commands.ReparentObject(body, worldObject);
        Check(objects.Children(worldObject).size() == 2U, 15, failure);
        Check(objects.Children(system).empty(), 16, failure);
        Check(objects.Find(body)->parent == worldObject, 17, failure);

        // A rolled-back transaction must not leave its writes cached.
        commands.BeginTransaction("Scratch");
        commands.SetProperty(atmo, kAtmosphereRayleighScaleHeightMeters, 99.0);
        {
            const auto value = objects.GetProperty(atmo, kAtmosphereRayleighScaleHeightMeters);
            Check(value.has_value() && std::get<f64>(*value) == 99.0, 18, failure);
        }
        commands.RollbackTransaction();
        Check(!objects.GetProperty(atmo, kAtmosphereRayleighScaleHeightMeters).has_value(), 19, failure);

        // Delete drops it from every read.
        commands.DeleteObject(atmo);
        Check(!objects.Find(atmo).has_value(), 20, failure);
        Check(objects.Children(body).empty(), 21, failure);
    }

    std::filesystem::remove_all(root);
    return failure;
}
