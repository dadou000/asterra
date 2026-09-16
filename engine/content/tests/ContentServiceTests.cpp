#include <orbit/content/ContentService.hpp>
#include <orbit/core/StrongId.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::abort();
    }
}

void Write(const std::filesystem::path& path, std::string_view text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    Check(output.good());
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
    Check(materials.size() == 1);
    Check(materials[0].name == "Brushed Steel");
    Check(materials[0].material.has_value());
    Check(materials[0].material->metallicFactor == 1.0);
    Check(materials[0].tags.size() == 2);
    Check(
        materials[0].sourceHash.ToHex().size() ==
        64);
    Check(materials[0].derivedReady);
    Check(materials[0].derivedKey.has_value());
    Check(
        content.Cache().Contains(
            *materials[0].derivedKey,
            "material.toml"));
    Check(materials[0].dependencies.size() == 3);

    const auto* baseTexture =
        content.FindByPath(
            "Content/Materials/steel_base.ktx2");

    Check(baseTexture != nullptr);

    const auto baseDependents =
        content.Dependents(
            baseTexture->id);

    Check(baseDependents.size() == 1);
    Check(
        baseDependents[0] ==
        materials[0].id);

    const auto firstDerivedKey =
        *materials[0].derivedKey;

    Check(content.Diagnostics().size() == 1);
    Check(content.Diagnostics()[0].sourcePath ==
        std::filesystem::path("Content/Materials/broken.orbitmaterial"));

    const auto stableId = materials[0].id;
    content.Scan();
    const auto* same = content.FindByPath("Content/Materials/steel.orbitmaterial");
    if (same == nullptr || same->id != stableId)
    {
        return 1;
    }

    Write(
        root /
            "Content" /
            "Materials" /
            "steel.orbitmaterial.orbitimport.toml",
        "[import]\n"
        "quality = \"high\"\n");

    content.Scan();

    const auto* withSettings =
        content.FindByPath(
            "Content/Materials/steel.orbitmaterial");

    Check(withSettings != nullptr);
    Check(withSettings->derivedKey.has_value());
    Check(
        *withSettings->derivedKey !=
        firstDerivedKey);

    const auto semanticSettingsKey =
        *withSettings->derivedKey;

    Write(
        root /
            "Content" /
            "Materials" /
            "steel.orbitmaterial.orbitimport.toml",
        "# formatting/comment-only edit\n"
        "[import]\n"
        "quality    =    \"high\"\n");

    content.Scan();

    const auto* reformattedSettings =
        content.FindByPath(
            "Content/Materials/steel.orbitmaterial");

    Check(reformattedSettings != nullptr);
    Check(reformattedSettings->derivedKey.has_value());
    Check(
        *reformattedSettings->derivedKey ==
        semanticSettingsKey);

    const auto pbrSource =
        root / "ExternalPbr";

    Write(
        pbrSource / "factory_albedo.png",
        "base");
    Write(
        pbrSource / "factory_normal.png",
        "normal");
    Write(
        pbrSource / "factory_roughness.png",
        "rough");
    Write(
        pbrSource / "factory_metallic.png",
        "metal");

    const auto pbrMaterialId =
        content.ImportPbrSet(
            pbrSource,
            "Factory Steel");

    const auto* pbrMaterial =
        content.Find(
            pbrMaterialId);

    Check(pbrMaterial != nullptr);
    Check(
        pbrMaterial->kind ==
        orbit::content::AssetKind::Material);
    Check(pbrMaterial->derivedReady);
    Check(
        pbrMaterial->dependencies.size() ==
        4);
    Check(
        pbrMaterial->material.has_value());
    Check(
        pbrMaterial->material->
            metallicFactor ==
        1.0);
    Check(
        std::filesystem::is_regular_file(
            root /
            pbrMaterial->sourcePath));

    const auto instanceId =
        content.CreateMaterialInstance(
            pbrMaterialId,
            "Factory Steel Wet");

    const auto* materialInstance =
        content.Find(
            instanceId);

    Check(materialInstance != nullptr);
    Check(
        materialInstance->kind ==
        orbit::content::
            AssetKind::MaterialInstance);
    Check(
        materialInstance->
            materialInstance.
            has_value());
    Check(materialInstance->derivedReady);
    Check(
        materialInstance->
            dependencies.size() ==
        1);
    Check(
        materialInstance->
            dependencies[0] ==
        pbrMaterialId);
    Check(
        content.Dependents(
            pbrMaterialId).
            size() >=
        1);

    const auto ambiguousPbr =
        root / "AmbiguousPbr";

    Write(
        ambiguousPbr / "part_normal.png",
        "normal-a");
    Write(
        ambiguousPbr / "part_normal_detail.png",
        "normal-b");

    bool ambiguousRejected = false;

    try
    {
        static_cast<void>(
            content.ImportPbrSet(
                ambiguousPbr,
                "Ambiguous"));
    }
    catch (const std::invalid_argument&)
    {
        ambiguousRejected = true;
    }

    Check(ambiguousRejected);

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

    const auto thumbnail =
        content.GetThumbnail(
            imported,
            32,
            24);

    Check(!thumbnail.cacheHit);
    Check(
        std::filesystem::is_regular_file(
            thumbnail.path));

    const auto cachedThumbnail =
        content.GetThumbnail(
            imported,
            32,
            24);

    Check(cachedThumbnail.cacheHit);
    Check(
        cachedThumbnail.key ==
        thumbnail.key);

    std::filesystem::remove_all(root);
    return 0;
}
