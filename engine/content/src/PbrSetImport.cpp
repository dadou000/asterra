#include <orbit/content/ContentService.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace orbit::content
{
namespace
{
enum class PbrChannel
{
    BaseColor,
    Normal,
    Roughness,
    Metallic,
    AmbientOcclusion,
    Emissive
};

struct ChannelCandidate
{
    PbrChannel channel{};
    std::filesystem::path source;
};

[[nodiscard]] std::string Lower(
    std::string value)
{
    std::ranges::transform(
        value,
        value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(
                std::tolower(character));
        });

    return value;
}

[[nodiscard]] bool IsTextureExtension(
    const std::filesystem::path& path)
{
    const std::string extension =
        Lower(
            path.extension().
                string());

    return
        extension == ".png" ||
        extension == ".jpg" ||
        extension == ".jpeg" ||
        extension == ".tga" ||
        extension == ".dds" ||
        extension == ".ktx2" ||
        extension == ".exr";
}

[[nodiscard]] bool ContainsAny(
    const std::string_view text,
    const std::initializer_list<
        std::string_view> needles)
{
    return std::ranges::any_of(
        needles,
        [text](
            const std::string_view needle)
        {
            return text.find(
                       needle) !=
                std::string_view::npos;
        });
}

[[nodiscard]] std::optional<PbrChannel>
DetectChannel(
    const std::filesystem::path& path)
{
    const std::string stem =
        Lower(
            path.stem().
                string());

    if (ContainsAny(
            stem,
            {
                "basecolor",
                "base_color",
                "base-color",
                "albedo",
                "diffuse"
            }))
    {
        return PbrChannel::BaseColor;
    }

    if (ContainsAny(
            stem,
            {
                "normal",
                "_nrm",
                "-nrm"
            }))
    {
        return PbrChannel::Normal;
    }

    if (ContainsAny(
            stem,
            {
                "roughness",
                "_rough",
                "-rough"
            }))
    {
        return PbrChannel::Roughness;
    }

    if (ContainsAny(
            stem,
            {
                "metallic",
                "metalness"
            }))
    {
        return PbrChannel::Metallic;
    }

    if (ContainsAny(
            stem,
            {
                "ambientocclusion",
                "ambient_occlusion",
                "ambient-occlusion",
                "_ao",
                "-ao"
            }))
    {
        return PbrChannel::AmbientOcclusion;
    }

    if (ContainsAny(
            stem,
            {
                "emissive",
                "emission"
            }))
    {
        return PbrChannel::Emissive;
    }

    return std::nullopt;
}

[[nodiscard]] const char*
ChannelDisplayName(
    const PbrChannel channel) noexcept
{
    switch (channel)
    {
    case PbrChannel::BaseColor:
        return "base color";
    case PbrChannel::Normal:
        return "normal";
    case PbrChannel::Roughness:
        return "roughness";
    case PbrChannel::Metallic:
        return "metallic";
    case PbrChannel::AmbientOcclusion:
        return "ambient occlusion";
    case PbrChannel::Emissive:
        return "emissive";
    }

    return "unknown";
}

[[nodiscard]] const char*
ChannelFileStem(
    const PbrChannel channel) noexcept
{
    switch (channel)
    {
    case PbrChannel::BaseColor:
        return "BaseColor";
    case PbrChannel::Normal:
        return "Normal";
    case PbrChannel::Roughness:
        return "Roughness";
    case PbrChannel::Metallic:
        return "Metallic";
    case PbrChannel::AmbientOcclusion:
        return "AmbientOcclusion";
    case PbrChannel::Emissive:
        return "Emissive";
    }

    return "Unknown";
}

[[nodiscard]] const char*
ChannelTomlKey(
    const PbrChannel channel) noexcept
{
    switch (channel)
    {
    case PbrChannel::BaseColor:
        return "base_color";
    case PbrChannel::Normal:
        return "normal";
    case PbrChannel::Roughness:
        return "roughness";
    case PbrChannel::Metallic:
        return "metallic";
    case PbrChannel::AmbientOcclusion:
        return "ambient_occlusion";
    case PbrChannel::Emissive:
        return "emissive";
    }

    return "unknown";
}

[[nodiscard]] std::string SanitizeName(
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
        result = "Material";
    }

    return result;
}

[[nodiscard]] std::filesystem::path
UniqueMaterialDirectory(
    const std::filesystem::path& root,
    const std::string& baseName)
{
    std::filesystem::path candidate =
        root /
        baseName;

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
            root /
            (baseName +
             "_" +
             std::to_string(
                 suffix));

        if (!std::filesystem::exists(
                candidate))
        {
            return candidate;
        }
    }

    throw std::runtime_error(
        "Unable to allocate a unique material directory.");
}
} // namespace

AssetId ContentService::ImportPbrSet(
    const std::filesystem::path& sourceDirectory,
    std::string materialName)
{
    if (!std::filesystem::is_directory(
            sourceDirectory))
    {
        throw std::invalid_argument(
            "PBR import source must be a directory.");
    }

    std::vector<ChannelCandidate>
        candidates;

    for (const auto& entry :
         std::filesystem::directory_iterator(
             sourceDirectory))
    {
        if (!entry.is_regular_file() ||
            !IsTextureExtension(
                entry.path()))
        {
            continue;
        }

        if (const auto channel =
                DetectChannel(
                    entry.path());
            channel.has_value())
        {
            candidates.push_back({
                .channel = *channel,
                .source =
                    entry.path()
            });
        }
    }

    if (candidates.empty())
    {
        throw std::invalid_argument(
            "PBR source directory contains no recognized channel maps.");
    }

    std::ranges::sort(
        candidates,
        [](const ChannelCandidate& left,
           const ChannelCandidate& right)
        {
            if (left.channel !=
                right.channel)
            {
                return left.channel <
                    right.channel;
            }

            return left.source.
                generic_string() <
                right.source.
                    generic_string();
        });

    for (std::size_t index = 1;
         index < candidates.size();
         ++index)
    {
        if (candidates[index - 1].
                channel ==
            candidates[index].
                channel)
        {
            throw std::invalid_argument(
                std::string(
                    "PBR source contains multiple ") +
                ChannelDisplayName(
                    candidates[index].
                        channel) +
                " maps: '" +
                candidates[index - 1].
                    source.
                    filename().
                    string() +
                "' and '" +
                candidates[index].
                    source.
                    filename().
                    string() +
                "'.");
        }
    }

    if (materialName.empty())
    {
        materialName =
            sourceDirectory.
                filename().
                string();
    }

    materialName =
        SanitizeName(
            std::move(
                materialName));

    const auto materialsRoot =
        contentRoot_ /
        "Materials";

    std::filesystem::create_directories(
        materialsRoot);

    const auto destinationDirectory =
        UniqueMaterialDirectory(
            materialsRoot,
            materialName);

    std::filesystem::create_directories(
        destinationDirectory);

    std::filesystem::path materialPath;

    try
    {
        toml::table material;
        material.insert(
            "name",
            materialName);
        material.insert(
            "roughness_factor",
            1.0);
        material.insert(
            "metallic_factor",
            0.0);

        toml::array tags;
        tags.push_back(
            "pbr");
        tags.push_back(
            "imported");

        material.insert(
            "tags",
            std::move(tags));

        bool hasMetallicMap = false;

        for (const auto& candidate :
             candidates)
        {
            const std::string extension =
                Lower(
                    candidate.source.
                        extension().
                        string());

            const std::string fileName =
                std::string(
                    ChannelFileStem(
                        candidate.channel)) +
                extension;

            const auto destination =
                destinationDirectory /
                fileName;

            std::filesystem::copy_file(
                candidate.source,
                destination);

            material.insert(
                ChannelTomlKey(
                    candidate.channel),
                fileName);

            if (candidate.channel ==
                PbrChannel::Metallic)
            {
                hasMetallicMap = true;
            }
        }

        if (hasMetallicMap)
        {
            material.insert_or_assign(
                "metallic_factor",
                1.0);
        }

        toml::table document;
        document.insert(
            "material",
            std::move(material));

        materialPath =
            destinationDirectory /
            (materialName +
             ".orbitmaterial");

        std::ofstream output(
            materialPath,
            std::ios::binary |
            std::ios::trunc);

        if (!output)
        {
            throw std::runtime_error(
                "Unable to create imported material asset.");
        }

        output << document;
        output.flush();

        if (!output)
        {
            throw std::runtime_error(
                "Unable to write imported material asset.");
        }
    }
    catch (...)
    {
        std::error_code error;
        std::filesystem::remove_all(
            destinationDirectory,
            error);
        throw;
    }

    Scan();

    const AssetRecord* imported =
        FindByPath(
            materialPath);

    if (imported == nullptr ||
        imported->kind !=
            AssetKind::Material)
    {
        throw std::runtime_error(
            "Imported PBR material did not enter the content registry.");
    }

    return imported->id;
}
} // namespace orbit::content
