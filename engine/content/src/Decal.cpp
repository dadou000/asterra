#include <orbit/content/ContentService.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace orbit::content
{
namespace
{
[[nodiscard]] std::vector<std::byte>
StringBytes(const std::string_view text)
{
    const auto bytes =
        std::as_bytes(
            std::span(
                text.data(),
                text.size()));

    return {
        bytes.begin(),
        bytes.end()
    };
}

[[nodiscard]] std::string SanitizeDecalName(
    std::string value)
{
    std::string result;
    result.reserve(value.size());

    bool previousSeparator = false;

    for (const unsigned char character : value)
    {
        if (std::isalnum(character) != 0 ||
            character == '-' ||
            character == '_')
        {
            result.push_back(
                static_cast<char>(character));
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
        result = "Decal";
    }

    return result;
}

[[nodiscard]] std::filesystem::path UniqueDecalPath(
    const std::filesystem::path& directory,
    const std::string& baseName)
{
    std::filesystem::path candidate =
        directory /
        (baseName + ".orbitdecal");

    if (!std::filesystem::exists(candidate))
    {
        return candidate;
    }

    for (u32 suffix = 2;
         suffix < std::numeric_limits<u32>::max();
         ++suffix)
    {
        candidate =
            directory /
            (baseName + "_" +
             std::to_string(suffix) +
             ".orbitdecal");

        if (!std::filesystem::exists(candidate))
        {
            return candidate;
        }
    }

    throw std::runtime_error(
        "Unable to allocate a unique decal path.");
}
} // namespace

void RegisterDecalImporter(
    ImporterRegistry& registry)
{
    registry.Register({
        .id = "orbit.decal",
        .version = 1,
        .extensions = {
            ".orbitdecal"
        },
        .import =
            [](const ImportRequest& request)
            {
                const toml::table document =
                    toml::parse_file(
                        request.sourcePath.string());

                const toml::table* decal =
                    document["decal"].as_table();

                if (decal == nullptr)
                {
                    throw std::runtime_error(
                        "Decal source is missing [decal].");
                }

                const auto texture =
                    (*decal)["texture"].
                        value<std::string>();

                if (!texture.has_value() ||
                    texture->empty())
                {
                    throw std::runtime_error(
                        "Decal source requires a texture dependency.");
                }

                std::ostringstream stream;
                stream << document;
                const std::string normalized =
                    stream.str();

                return ImportOutput{
                    .artifacts = {
                        {
                            .name = "decal.toml",
                            .bytes = StringBytes(
                                normalized)
                        }
                    },
                    .dependencies = {
                        std::filesystem::path(*texture)
                    }
                };
            }
    });
}

AssetId ContentService::CreateDecal(
    const AssetId textureAsset,
    std::string decalName,
    const f64 widthMeters,
    const f64 heightMeters,
    const f64 opacity)
{
    const AssetRecord* texture =
        Find(textureAsset);

    if (texture == nullptr ||
        texture->kind != AssetKind::Texture)
    {
        throw std::invalid_argument(
            "Decals require a Texture asset.");
    }

    if (!(widthMeters > 0.0) ||
        !(heightMeters > 0.0))
    {
        throw std::invalid_argument(
            "Decal width and height must be positive.");
    }

    if (opacity < 0.0 || opacity > 1.0)
    {
        throw std::invalid_argument(
            "Decal opacity must be in the [0, 1] range.");
    }

    if (decalName.empty())
    {
        decalName = texture->name + " Decal";
    }

    decalName =
        SanitizeDecalName(
            std::move(decalName));

    const auto directory =
        contentRoot_ / "Decals";

    std::filesystem::create_directories(
        directory);

    const auto destination =
        UniqueDecalPath(
            directory,
            decalName);

    const auto textureAbsolute =
        projectRoot_ /
        texture->sourcePath;

    const auto relativeTexture =
        std::filesystem::relative(
            textureAbsolute,
            destination.parent_path());

    toml::table decal;
    decal.insert("name", decalName);
    decal.insert(
        "texture",
        relativeTexture.generic_string());
    decal.insert("width_meters", widthMeters);
    decal.insert("height_meters", heightMeters);
    decal.insert("opacity", opacity);

    toml::array tags;
    tags.push_back("decal");
    decal.insert("tags", std::move(tags));

    toml::table document;
    document.insert("decal", std::move(decal));

    std::ofstream output(
        destination,
        std::ios::binary |
        std::ios::trunc);

    if (!output)
    {
        throw std::runtime_error(
            "Unable to create decal asset.");
    }

    output << document;
    output.flush();

    if (!output)
    {
        std::filesystem::remove(destination);
        throw std::runtime_error(
            "Unable to write decal asset.");
    }

    Scan();

    const AssetRecord* created =
        FindByPath(destination);

    if (created == nullptr ||
        created->kind != AssetKind::Decal ||
        !created->decal.has_value())
    {
        throw std::runtime_error(
            "Created decal did not enter the content registry.");
    }

    return created->id;
}
} // namespace orbit::content
