#include <orbit/commands/CommandService.hpp>
#include <orbit/content/ContentService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/studio_ui/LightingSelectionInspection.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <array>
#include <cmath>
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

// Everything that opens the project database lives in Run(): remove_all() throws on
// Windows while SQLite still holds the files open, so it must run after Run() returns.
int Run(const std::filesystem::path& root)
{
    using namespace orbit;

    auto project =
        documents::ProjectDocument::Create(
            root,
            "M41 Lighting Selection");

    Write(
        root /
            "Content" /
            "Materials" /
            "led-panel.orbitmaterial",
        "[material]\n"
        "name = \"M41 LED Panel\"\n"
        "emission_color_linear = [0.1, 0.4, 1.0]\n"
        "emission_luminance_nits = 2400.0\n"
        "emission_gi_enabled = true\n"
        "emission_gi_scale = 0.65\n");

    content::ContentService content(root);
    content.Scan();

    const auto materials =
        content.Search(
            "M41 LED Panel",
            content::AssetKind::Material);

    if (materials.size() != 1U)
    {
        return 1;
    }

    documents::WorldDatabase world(
        project.StartupWorldPath());

    schema::SchemaRegistry schemas;
    world_model::RegisterSchemas(schemas);

    scene::ObjectStore objects(world);
    commands::CommandService commands(
        objects,
        schemas);

    const auto owner =
        commands.CreateObject(
            world_model::kGeologyAssetType,
            "LED Wall");

    const auto assignment =
        commands.CreateObject(
            world_model::kMaterialAssignmentType,
            "LED Wall Material",
            owner);

    commands.SetProperty(
        assignment,
        world_model::kMaterialAssignmentAsset,
        materials.front().id.ToString());
    commands.SetProperty(
        assignment,
        world_model::kMaterialAssignmentSlot,
        std::string{"body"});

    const std::array selection{owner};

    const auto inspected =
        studio_ui::InspectSelectedLightingAuthority(
            objects,
            selection,
            content);

    if (!inspected.hasSelection ||
        inspected.selectedObject != "LED Wall" ||
        !inspected.selectedEmissive ||
        !inspected.emissionGiEnabled ||
        inspected.emissionLuminanceNits != 2400.0 ||
        // The material stores the GI scale as f32, so 0.65 comes back as 0.65F.
        std::abs(inspected.emissionGiScale - 0.65) > 1.0e-6)
    {
        return 2;
    }

    const std::array<scene::ObjectId, 0U> emptySelection{};
    const auto empty =
        studio_ui::InspectSelectedLightingAuthority(
            objects,
            emptySelection,
            content);

    if (empty.hasSelection ||
        empty.selectedEmissive)
    {
        return 3;
    }

    commands.SetProperty(
        assignment,
        world_model::kMaterialAssignmentAsset,
        std::string{
            "01234567-89ab-cdef-0123-456789abcdef"});

    const auto stale =
        studio_ui::InspectSelectedLightingAuthority(
            objects,
            selection,
            content);

    if (!stale.hasSelection ||
        stale.selectedEmissive ||
        stale.emissionGiEnabled ||
        stale.emissionLuminanceNits != 0.0)
    {
        return 4;
    }

    return 0;
}
} // namespace

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-m41-selection-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);
    const int result = Run(root);
    std::filesystem::remove_all(root);
    return result;
}
