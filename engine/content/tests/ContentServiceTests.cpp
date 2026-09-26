#include <orbit/content/ContentService.hpp>
#include <orbit/core/StrongId.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>

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
            << "ContentService test failed at "
            << location.file_name()
            << ':'
            << location.line()
            << '\n';
        std::exit(1);
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
        "emission_color_linear = [0.25, 0.5, 1.0]\n"
        "emission_luminance_nits = 1200.0\n"
        "emission_gi_enabled = true\n"
        "emission_gi_scale = 0.75\n"
        "tags = [\"metal\", \"industrial\"]\n");
    Write(root / "Content" / "Materials" / "steel_base.ktx2", "texture");
    Write(root / "Content" / "Materials" / "steel_normal.ktx2", "texture");
    Write(root / "Content" / "Materials" / "steel_rough.ktx2", "texture");
    Write(root / "Content" / "Materials" / "broken.orbitmaterial",
        "[not_material]\nname = \"Broken\"\n");

    Write(
        root / "Content" / "Paths" / "road.orbitpathprofile",
        "[path_profile]\n"
        "name = \"Regional Road\"\n"
        "kind = \"road\"\n"
        "width_meters = 7.0\n"
        "lanes = 2\n");

    orbit::content::ContentService content(root);
    content.Scan();
    const auto materials = content.Search("steel", orbit::content::AssetKind::Material);
    Check(materials.size() == 1);
    Check(materials[0].name == "Brushed Steel");
    Check(materials[0].material.has_value());
    Check(materials[0].material->metallicFactor == 1.0);
    Check(materials[0].material->emission.luminanceNits == 1200.0);
    Check(materials[0].material->emission.colorLinear[2] == 1.0);
    Check(materials[0].material->emission.contributesToGi);
    Check(materials[0].material->emission.giScale == 0.75);
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

    const auto pathProfiles =
        content.Search(
            "regional",
            orbit::content::AssetKind::PathProfile);

    Check(pathProfiles.size() == 1);
    Check(pathProfiles[0].name == "Regional Road");
    Check(pathProfiles[0].tags.size() == 2);
    Check(pathProfiles[0].tags[0] == "path");
    Check(pathProfiles[0].tags[1] == "road");
    Check(
        orbit::content::AssetKindName(
            pathProfiles[0].kind) ==
        "Path Profile");

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
    const auto stablePathProfileId =
        pathProfiles[0].id;
    content.Scan();
    const auto* same = content.FindByPath("Content/Materials/steel.orbitmaterial");
    if (same == nullptr || same->id != stableId)
    {
        return 1;
    }

    const auto* samePathProfile =
        content.FindByPath(
            "Content/Paths/road.orbitpathprofile");

    Check(samePathProfile != nullptr);
    Check(
        samePathProfile->id ==
        stablePathProfileId);

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

    content.SetMaterialEmission(
        pbrMaterialId,
        {
            .colorLinear = {0.1, 0.8, 0.3},
            .luminanceNits = 3500.0,
            .contributesToGi = false,
            .giScale = 2.0
        });

    const auto* editedBase =
        content.Find(pbrMaterialId);

    Check(editedBase != nullptr);
    Check(editedBase->material.has_value());
    Check(
        editedBase->material->emission.
            luminanceNits == 3500.0);
    Check(
        editedBase->material->emission.
            colorLinear[1] == 0.8);
    Check(
        !editedBase->material->emission.
            contributesToGi);
    Check(
        editedBase->material->emission.
            giScale == 2.0);

    content.SetMaterialEmission(
        instanceId,
        {
            .colorLinear = {1.0, 0.25, 0.05},
            .luminanceNits = 900.0,
            .contributesToGi = true,
            .giScale = 0.4
        });

    const auto* editedInstance =
        content.Find(instanceId);

    Check(editedInstance != nullptr);
    Check(
        editedInstance->materialInstance.
            has_value());
    Check(
        editedInstance->materialInstance->
            emissionLuminanceNits.
            value_or(-1.0) == 900.0);
    Check(
        editedInstance->materialInstance->
            emissionColorLinear.
            has_value());
    Check(
        (*editedInstance->materialInstance->
            emissionColorLinear)[0] == 1.0);
    Check(
        editedInstance->materialInstance->
            emissionContributesToGi.
            value_or(false));
    Check(
        editedInstance->materialInstance->
            emissionGiScale.
            value_or(-1.0) == 0.4);

    // Base-material emissive texture is independently editable from
    // physical luminance/GI policy.
    Write(
        root /
            "Content" /
            "Materials" /
            "Instances" /
            "PanelGlow.png",
        "emissive");

    content.SetMaterialEmissiveTexture(
        pbrMaterialId,
        std::filesystem::path(
            "../Instances/PanelGlow.png"));

    const auto* texturedMaterial =
        content.Find(
            pbrMaterialId);

    Check(texturedMaterial != nullptr);
    Check(texturedMaterial->material.has_value());
    Check(
        texturedMaterial->material->emissive ==
        std::filesystem::path(
            "../Instances/PanelGlow.png"));

    content.SetMaterialEmissiveTexture(
        pbrMaterialId,
        {});

    const auto* clearedEmissionTexture =
        content.Find(
            pbrMaterialId);

    Check(clearedEmissionTexture != nullptr);
    Check(
        clearedEmissionTexture->material.
            has_value());
    Check(
        clearedEmissionTexture->material->
            emissive.empty());

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

    const auto decalId =
        content.CreateDecal(
            imported,
            "Workshop Mark",
            2.5,
            1.25,
            0.8);

    const auto* decal =
        content.Find(decalId);

    Check(decal != nullptr);
    Check(decal->kind == orbit::content::AssetKind::Decal);
    Check(decal->decal.has_value());
    Check(decal->decal->widthMeters == 2.5);
    Check(decal->decal->heightMeters == 1.25);
    Check(decal->decal->opacity == 0.8);
    Check(decal->derivedReady);
    Check(decal->derivedKey.has_value());
    Check(
        content.Cache().Contains(
            *decal->derivedKey,
            "decal.toml"));
    Check(decal->dependencies.size() == 1);
    Check(decal->dependencies[0] == imported);

    const auto decalDependents =
        content.Dependents(imported);
    Check(
        std::find(
            decalDependents.begin(),
            decalDependents.end(),
            decalId) !=
        decalDependents.end());

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

    // ---- Shading tab: folders, shaders, shader materials ------------------
    using orbit::content::AssetKind;

    const auto revisionBeforeFolder = content.Revision();
    content.CreateFolder("Content/Shading");
    content.CreateFolder("Content/Shading/Lunar");
    Check(content.Revision() > revisionBeforeFolder);
    Check(std::filesystem::is_directory(root / "Content" / "Shading" / "Lunar"));

    bool sawLunarFolder = false;
    for (const auto& folder : content.Folders())
    {
        sawLunarFolder |= folder.generic_string() == "Content/Shading/Lunar";
    }
    Check(sawLunarFolder);

    // Duplicate, escaping and reserved-name creation is refused.
    bool refused = false;
    try { content.CreateFolder("Content/Shading"); }
    catch (const std::invalid_argument&) { refused = true; }
    Check(refused);
    refused = false;
    try { content.CreateFolder("Content/../Escape"); }
    catch (const std::invalid_argument&) { refused = true; }
    Check(refused);
    refused = false;
    try { content.CreateFolder("Content/Shading/bad:name"); }
    catch (const std::invalid_argument&) { refused = true; }
    Check(refused);

    content.WriteText(
        "Content/Shading/Lunar/Regolith.shade.hlsl",
        "float4 Shade(OrbitSurface s, OrbitLighting l) { return 1; }\n");
    Check(content.ReadText("Content/Shading/Lunar/Regolith.shade.hlsl")
              .find("Shade(") != std::string::npos);

    const auto* shadingShader =
        content.FindByPath("Content/Shading/Lunar/Regolith.shade.hlsl");
    Check(shadingShader != nullptr);
    Check(shadingShader->kind == AssetKind::ShadingShader);
    Check(shadingShader->name == "Regolith");
    // Not a standalone HLSL stage: no sidecar required, no diagnostic raised.
    for (const auto& diagnostic : content.Diagnostics())
    {
        Check(diagnostic.sourcePath.generic_string().find("Regolith") ==
              std::string::npos);
    }

    content.WriteText(
        "Content/Shading/Lunar/Regolith.orbitshadermaterial",
        "[shader_material]\n"
        "name = \"Lunar Regolith\"\n"
        "shader = \"Regolith.shade.hlsl\"\n"
        "\n"
        "[[shader_material.parameter]]\n"
        "name = \"tint\"\n"
        "value = [0.5, 0.5, 0.45]\n"
        "\n"
        "[[shader_material.parameter]]\n"
        "name = \"roughness\"\n"
        "value = 0.9\n");

    const auto* shaderMaterial =
        content.FindByPath("Content/Shading/Lunar/Regolith.orbitshadermaterial");
    Check(shaderMaterial != nullptr);
    Check(shaderMaterial->kind == AssetKind::ShaderMaterial);
    Check(shaderMaterial->name == "Lunar Regolith");
    Check(shaderMaterial->shaderMaterial.has_value());
    // Parameter declaration order is preserved (it is the packing order).
    Check(shaderMaterial->shaderMaterial->parameters.size() == 2);
    Check(shaderMaterial->shaderMaterial->parameters[0].name == "tint");
    Check(shaderMaterial->shaderMaterial->parameters[0].values.size() == 3);
    Check(shaderMaterial->shaderMaterial->parameters[1].values[0] == 0.9);
    // It depends on its shader, so a shader edit refreshes dependents.
    Check(shaderMaterial->dependencies.size() == 1);
    Check(shaderMaterial->dependencies[0] == shadingShader->id);

    // Rename and move keep files inside Content and refuse collisions.
    const auto renamed = content.RenameEntry(
        "Content/Shading/Lunar/Regolith.shade.hlsl", "Dust.shade.hlsl");
    Check(renamed.generic_string() == "Content/Shading/Lunar/Dust.shade.hlsl");
    Check(content.FindByPath(renamed) != nullptr);
    Check(content.FindByPath("Content/Shading/Lunar/Regolith.shade.hlsl") == nullptr);

    content.CreateFolder("Content/Shading/Archive");
    const auto moved = content.MoveEntry(renamed, "Content/Shading/Archive");
    Check(moved.generic_string() == "Content/Shading/Archive/Dust.shade.hlsl");

    refused = false;
    try { content.MoveEntry("Content/Shading", "Content/Shading/Lunar"); }
    catch (const std::invalid_argument&) { refused = true; }
    Check(refused);

    // Trash is reversible: the file lands under .orbit/Trash, not deleted.
    const auto trashed = content.TrashEntry(moved);
    Check(!std::filesystem::exists(root / moved));
    Check(std::filesystem::is_regular_file(root / trashed));
    Check(trashed.generic_string().starts_with(".orbit/Trash/"));

    // Atomic write leaves no temporary file behind.
    for (const auto& item :
         std::filesystem::recursive_directory_iterator(root / "Content"))
    {
        Check(item.path().extension() != ".tmp");
    }

    std::filesystem::remove_all(root);
    return 0;
}
