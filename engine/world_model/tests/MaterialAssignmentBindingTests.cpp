#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/world_model/MaterialAssignmentBinding.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <filesystem>

int main()
{
    using namespace orbit;

    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-material-assignment-" +
         documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            documents::ProjectDocument::Create(
                root,
                "Material Assignment Test");

        documents::WorldDatabase world(
            project.StartupWorldPath());

        schema::SchemaRegistry schemas;
        world_model::RegisterSchemas(schemas);

        if (schemas.FindType(
                world_model::kMaterialAssignmentType) ==
            nullptr)
        {
            return 1;
        }

        scene::ObjectStore objects(world);
        commands::CommandService commands(
            objects,
            schemas);

        const auto owner =
            commands.CreateObject(
                world_model::kGeologyAssetType,
                "Renderable Owner");

        const auto assignment =
            commands.CreateObject(
                world_model::kMaterialAssignmentType,
                "Body Material",
                owner);

        commands.SetProperty(
            assignment,
            world_model::kMaterialAssignmentAsset,
            std::string{
                "01234567-89ab-cdef-0123-456789abcdef"});

        commands.SetProperty(
            assignment,
            world_model::kMaterialAssignmentSlot,
            std::string{"body"});

        auto resolved =
            world_model::ResolveMaterialAssignments(
                objects,
                owner);

        if (resolved.size() != 1U ||
            resolved.front().owner != owner ||
            resolved.front().assignment != assignment ||
            resolved.front().slot != "body")
        {
            return 2;
        }

        commands.SetProperty(
            assignment,
            world_model::kMaterialAssignmentEnabled,
            false);

        if (!world_model::
                ResolveMaterialAssignments(
                    objects,
                    owner).empty())
        {
            return 3;
        }

        commands.Undo();

        resolved =
            world_model::ResolveMaterialAssignments(
                objects,
                owner);

        if (resolved.size() != 1U)
        {
            return 4;
        }
    }

    std::filesystem::remove_all(root);
    return 0;
}
