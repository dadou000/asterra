#include <orbit/content/ContentService.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace orbit::content
{
namespace
{
[[nodiscard]] std::string SanitizeInstanceName(
    std::string value)
{
    std::string result;
    result.reserve(
        value.size());

    bool previousSeparator = false;

    for (const unsigned char character :
         value)
    {
        if (std::isalnum(character) != 0 ||
            character == '-' ||
            character == '_')
        {
            result.push_back(
                static_cast<char>(
                    character));
            previousSeparator = false;
        }
        else if (!previousSeparator &&
                 !result.empty())
        {
            result.push_back('_');
            previousSeparator = true;
        }
    }

    while (!result.empty() &&
           result.back() == '_')
    {
        result.pop_back();
    }

    if (result.empty())
    {
        result = "MaterialInstance";
    }

    return result;
}

[[nodiscard]] std::filesystem::path
UniqueInstancePath(
    const std::filesystem::path& directory,
    const std::string& baseName)
{
    std::filesystem::path candidate =
        directory /
        (baseName +
         ".orbitmaterialinstance");

    if (!std::filesystem::exists(
            candidate))
    {
        return candidate;
    }

    for (u32 suffix = 2;
         suffix <
             std::numeric_limits<u32>::max();
         ++suffix)
    {
        candidate =
            directory /
            (baseName +
             "_" +
             std::to_string(
                 suffix) +
             ".orbitmaterialinstance");

        if (!std::filesystem::exists(
                candidate))
        {
            return candidate;
        }
    }

    throw std::runtime_error(
        "Unable to allocate a unique material-instance path.");
}
} // namespace

AssetId ContentService::CreateMaterialInstance(
    const AssetId baseMaterial,
    std::string instanceName)
{
    const AssetRecord* base =
        Find(
            baseMaterial);

    if (base == nullptr ||
        base->kind !=
            AssetKind::Material)
    {
        throw std::invalid_argument(
            "Material instances require a base Material asset.");
    }

    if (instanceName.empty())
    {
        instanceName =
            base->name +
            " Instance";
    }

    instanceName =
        SanitizeInstanceName(
            std::move(
                instanceName));

    const auto directory =
        contentRoot_ /
        "Materials" /
        "Instances";

    std::filesystem::create_directories(
        directory);

    const auto destination =
        UniqueInstancePath(
            directory,
            instanceName);

    const auto baseAbsolute =
        projectRoot_ /
        base->sourcePath;

    const auto relativeParent =
        std::filesystem::relative(
            baseAbsolute,
            destination.
                parent_path());

    toml::table instance;
    instance.insert(
        "name",
        instanceName);
    instance.insert(
        "parent",
        relativeParent.
            generic_string());

    toml::array tags;
    tags.push_back(
        "material-instance");
    instance.insert(
        "tags",
        std::move(tags));

    toml::table document;
    document.insert(
        "material_instance",
        std::move(instance));

    std::ofstream output(
        destination,
        std::ios::binary |
        std::ios::trunc);

    if (!output)
    {
        throw std::runtime_error(
            "Unable to create material-instance asset.");
    }

    output << document;
    output.flush();

    if (!output)
    {
        std::filesystem::remove(
            destination);
        throw std::runtime_error(
            "Unable to write material-instance asset.");
    }

    Scan();

    const AssetRecord* created =
        FindByPath(
            destination);

    if (created == nullptr ||
        created->kind !=
            AssetKind::MaterialInstance)
    {
        throw std::runtime_error(
            "Created material instance did not enter the content registry.");
    }

    return created->id;
}

void ContentService::SetMaterialEmission(
    const AssetId materialAsset,
    const MaterialEmission& emission)
{
    const AssetRecord* asset =
        Find(materialAsset);

    if (asset == nullptr ||
        (asset->kind != AssetKind::Material &&
         asset->kind != AssetKind::MaterialInstance))
    {
        throw std::invalid_argument(
            "Physical emission can only be edited on Material or Material Instance assets.");
    }

    if (!std::isfinite(emission.luminanceNits) ||
        emission.luminanceNits < 0.0 ||
        !std::isfinite(emission.giScale) ||
        emission.giScale < 0.0)
    {
        throw std::invalid_argument(
            "Emission luminance and GI scale must be finite and non-negative.");
    }

    for (const f64 channel : emission.colorLinear)
    {
        if (!std::isfinite(channel) ||
            channel < 0.0)
        {
            throw std::invalid_argument(
                "Emission color must be finite and non-negative.");
        }
    }

    const auto absolute =
        projectRoot_ /
        asset->sourcePath;

    toml::table document =
        toml::parse_file(
            absolute.string());

    const char* tableName =
        asset->kind == AssetKind::Material
            ? "material"
            : "material_instance";

    toml::table* table =
        document[tableName].as_table();

    if (table == nullptr)
    {
        throw std::runtime_error(
            "Material authority table is missing while editing emission.");
    }

    toml::array color;
    color.push_back(emission.colorLinear[0]);
    color.push_back(emission.colorLinear[1]);
    color.push_back(emission.colorLinear[2]);

    table->insert_or_assign(
        "emission_color_linear",
        std::move(color));

    table->insert_or_assign(
        "emission_luminance_nits",
        emission.luminanceNits);

    table->insert_or_assign(
        "emission_gi_enabled",
        emission.contributesToGi);

    table->insert_or_assign(
        "emission_gi_scale",
        emission.giScale);

    std::ofstream output(
        absolute,
        std::ios::binary |
        std::ios::trunc);

    if (!output)
    {
        throw std::runtime_error(
            "Unable to open material asset for emission update.");
    }

    output << document;
    output.flush();

    if (!output)
    {
        throw std::runtime_error(
            "Unable to persist material emission update.");
    }

    Scan();
}

} // namespace orbit::content
