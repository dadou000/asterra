#include <orbit/commands/CommandService.hpp>
#include <orbit/content/ContentService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/lighting/RuntimeMaterialEmission.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/world_model/MaterialAssignmentBinding.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <filesystem>
#include <fstream>

namespace
{
void Write(
    const std::filesystem::path& path,
    const std::string_view text)
{
    std::filesystem::create_directories(
        path.parent_path());

    std::ofstream output(
        path,
        std::ios::binary |
        std::ios::trunc);

    output << text;
}
} // namespace

int main()
{
    using namespace orbit;

    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-runtime-material-assignment-" +
         documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    Write(
        root /
            "Content" /
            "Materials" /
            "lamp.orbitmaterial",
        "[material]\n"
        "name = \"Lamp Base\"\n"
        "emission_color_linear = [1.0, 0.4, 0.1]\n"
        "emission_luminance_nits = 6830.0\n"
        "emission_gi_enabled = true\n"
        "emission_gi_scale = 0.25\n");

    auto project =
        documents::ProjectDocument::Create(
            root,
            "Runtime Material Assignment Test");

    documents::WorldDatabase worldDatabase(
        project.StartupWorldPath());

    schema::SchemaRegistry schemas;
    world_model::RegisterSchemas(schemas);

    scene::ObjectStore objects(worldDatabase);
    commands::CommandService commands(
        objects,
        schemas);

    content::ContentService content(root);
    content.Scan();

    const auto materials =
        content.Search(
            "Lamp Base",
            content::AssetKind::Material);

    if (materials.size() != 1U)
    {
        return 1;
    }

    const auto owner =
        commands.CreateObject(
            world_model::kGeologyAssetType,
            "Runtime Object");

    const auto assignment =
        commands.CreateObject(
            world_model::kMaterialAssignmentType,
            "Material Assignment",
            owner);

    commands.SetProperty(
        assignment,
        world_model::kMaterialAssignmentAsset,
        materials.front().id.ToString());

    const auto resolved =
        world_model::ResolveMaterialAssignments(
            objects,
            owner);

    if (resolved.size() != 1U ||
        resolved.front().owner != owner ||
        resolved.front().slot != "default")
    {
        return 2;
    }

    lighting::SurfaceData surface;

    lighting::ApplyRuntimeMaterialEmission(
        surface,
        content,
        resolved.front().assetId);

    if (surface.emissionRadianceSceneLinear.x <=
            0.0F ||
        surface.emissionGiScale !=
            0.25F)
    {
        return 3;
    }

    const auto before =
        surface.emissionRadianceSceneLinear.x;

    content.SetMaterialEmission(
        materials.front().id,
        {
            .colorLinear = {0.2, 1.0, 0.3},
            .luminanceNits = 13'660.0,
            .contributesToGi = false,
            .giScale = 4.0
        });

    lighting::ApplyRuntimeMaterialEmission(
        surface,
        content,
        resolved.front().assetId);

    if (surface.emissionRadianceSceneLinear.x <=
            before ||
        surface.emissionGiScale !=
            0.0F)
    {
        return 4;
    }

    std::filesystem::remove_all(root);
    return 0;
}
