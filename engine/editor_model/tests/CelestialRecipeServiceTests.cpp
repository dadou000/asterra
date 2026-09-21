#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/editor_model/CelestialRecipeService.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/UniverseComposition.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

namespace
{
double FloatProperty(
    const orbit::scene::ObjectStore& objects,
    const orbit::scene::ObjectId object,
    const orbit::schema::PropertyId property)
{
    const auto value =
        objects.GetProperty(
            object,
            property);

    if (!value.has_value())
    {
        throw std::runtime_error(
            "Expected recipe float property is missing.");
    }

    return std::get<double>(*value);
}

orbit::scene::ObjectId ChildOfType(
    const orbit::scene::ObjectStore& objects,
    const orbit::scene::ObjectId parent,
    const orbit::schema::TypeId type)
{
    for (const auto& child :
         objects.Children(parent))
    {
        if (child.type == type)
        {
            return child.id;
        }
    }

    throw std::runtime_error(
        "Expected recipe child type is missing.");
}
} // namespace

int main()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-celestial-recipes-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Celestial Recipe Test");
        orbit::documents::WorldDatabase world(
            project.StartupWorldPath());
        orbit::schema::SchemaRegistry schemas;
        orbit::world_model::RegisterSchemas(schemas);
        orbit::scene::ObjectStore objects(world);
        orbit::commands::CommandService commands(
            objects,
            schemas);

        const auto worldObject =
            commands.CreateObject(
                orbit::world_model::kWorldType,
                "World");

        orbit::editor_model::CelestialRecipeService
            recipes(
                objects,
                commands);

        const orbit::editor_model::SeededSystemRecipe
            recipe{
                .seed = 0x12345678ULL,
                .systemName = "Deterministic",
                .starName = "Primary",
                .rockyPlanetCount = 5U,
                .generateMoons = true
            };

        const auto first =
            recipes.CreateSeededSystem(
                worldObject,
                recipe);

        if (first.planets.size() != 5U ||
            !objects.Find(first.system).has_value() ||
            !objects.Find(first.star).has_value())
        {
            return 1;
        }

        const auto firstPlanet =
            first.planets.front();
        const auto firstOrbit =
            ChildOfType(
                objects,
                firstPlanet,
                orbit::world_model::
                    kOrbitCapabilityType);

        const double firstRadius =
            FloatProperty(
                objects,
                firstPlanet,
                orbit::world_model::kBodyRadius);
        const double firstMass =
            FloatProperty(
                objects,
                firstPlanet,
                orbit::world_model::kBodyMass);
        const double firstAxis =
            FloatProperty(
                objects,
                firstOrbit,
                orbit::world_model::
                    kOrbitSemiMajorAxisMeters);
        const double firstEccentricity =
            FloatProperty(
                objects,
                firstOrbit,
                orbit::world_model::
                    kOrbitEccentricity);

        orbit::world_model::UniverseComposition
            universe;
        const auto stats =
            universe.Rebuild(objects);

        if (stats.systems != 1U ||
            stats.bodies !=
                1U +
                first.planets.size() +
                first.moons.size())
        {
            return 2;
        }

        for (const auto moon :
             first.moons)
        {
            const auto moonId =
                universe.BodyForObject(moon);

            const auto moonRecord =
                objects.Find(moon);

            if (!moonId.has_value() ||
                !moonRecord.has_value() ||
                !moonRecord->parent.has_value())
            {
                return 3;
            }

            const auto parentBodyId =
                universe.BodyForObject(
                    *moonRecord->parent);

            const auto* moonBody =
                universe.Bodies().FindBody(
                    *moonId);
            const auto* parentBody =
                parentBodyId.has_value()
                    ? universe.Bodies().FindBody(
                          *parentBodyId)
                    : nullptr;

            if (moonBody == nullptr ||
                parentBody == nullptr ||
                moonBody->parentFrame !=
                    parentBody->centerFrame)
            {
                return 4;
            }
        }

        commands.Undo();

        if (objects.Find(first.system).has_value())
        {
            return 5;
        }

        commands.Redo();

        if (!objects.Find(first.system).has_value() ||
            !objects.Find(firstPlanet).has_value())
        {
            return 6;
        }

        const auto second =
            recipes.CreateSeededSystem(
                worldObject,
                recipe);

        const auto secondPlanet =
            second.planets.front();
        const auto secondOrbit =
            ChildOfType(
                objects,
                secondPlanet,
                orbit::world_model::
                    kOrbitCapabilityType);

        if (FloatProperty(
                objects,
                secondPlanet,
                orbit::world_model::kBodyRadius) !=
                firstRadius ||
            FloatProperty(
                objects,
                secondPlanet,
                orbit::world_model::kBodyMass) !=
                firstMass ||
            FloatProperty(
                objects,
                secondOrbit,
                orbit::world_model::
                    kOrbitSemiMajorAxisMeters) !=
                firstAxis ||
            FloatProperty(
                objects,
                secondOrbit,
                orbit::world_model::
                    kOrbitEccentricity) !=
                firstEccentricity)
        {
            return 7;
        }

        const auto starGravity =
            ChildOfType(
                objects,
                first.star,
                orbit::world_model::
                    kGravityCapabilityType);
        const auto starEmitter =
            ChildOfType(
                objects,
                first.star,
                orbit::world_model::
                    kRadiativeEmitterCapabilityType);
        const auto planetSurface =
            ChildOfType(
                objects,
                firstPlanet,
                orbit::world_model::
                    kSurfaceCapabilityType);

        if (!starGravity ||
            !starEmitter ||
            !planetSurface)
        {
            return 8;
        }

        world.Checkpoint();
    }

    std::filesystem::remove_all(root);
    return 0;
}
