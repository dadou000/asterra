#include <orbit/content/ContentService.hpp>
#include <orbit/core/StrongId.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>

namespace
{
void Write(const std::filesystem::path& path, std::string_view text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output.good());
    output << text;
}
}

int main()
{
    const auto root = std::filesystem::temp_directory_path() /
        ("orbit-content-" + orbit::core::StrongId<struct TestTag>::Random().ToString());
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Content" / "Materials");

    Write(root / "Content" / "Materials" / "steel.orbitmaterial",
        "[material]\n"
        "name = \"Brushed Steel\"\n"
        "base_color = \"steel_base.ktx2\"\n"
        "normal = \"steel_normal.ktx2\"\n"
        "roughness = \"steel_rough.ktx2\"\n"
        "metallic_factor = 1.0\n"
        "roughness_factor = 0.32\n"
        "tags = [\"metal\", \"industrial\"]\n");
    Write(root / "Content" / "Materials" / "steel_base.ktx2", "texture");
    Write(root / "Content" / "Materials" / "steel_normal.ktx2", "texture");
    Write(root / "Content" / "Materials" / "steel_rough.ktx2", "texture");
    Write(root / "Content" / "Materials" / "broken.orbitmaterial",
        "[not_material]\nname = \"Broken\"\n");

    orbit::content::ContentService content(root);
    content.Scan();
    const auto materials = content.Search("steel", orbit::content::AssetKind::Material);
    assert(materials.size() == 1);
    assert(materials[0].name == "Brushed Steel");
    assert(materials[0].material.has_value());
    assert(materials[0].material->metallicFactor == 1.0);
    assert(materials[0].tags.size() == 2);
    assert(content.Diagnostics().size() == 1);
    assert(content.Diagnostics()[0].sourcePath ==
        std::filesystem::path("Content/Materials/broken.orbitmaterial"));

    const auto stableId = materials[0].id;
    content.Scan();
    const auto* same = content.FindByPath("Content/Materials/steel.orbitmaterial");
    if (same == nullptr || same->id != stableId)
    {
        return 1;
    }

    const auto external = root / "source.png";
    Write(external, "image");
    const auto imported = content.ImportFile(external);
    const auto* importedRecord = content.Find(imported);
    if (importedRecord == nullptr ||
        importedRecord->kind != orbit::content::AssetKind::Texture ||
        importedRecord->sourcePath !=
            std::filesystem::path("Content/Imported/source.png"))
    {
        return 1;
    }

    std::filesystem::remove_all(root);
    return 0;
}
