#include <orbit/content/ContentService.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace orbit::content
{
namespace
{
[[nodiscard]] u64 Hash(std::string_view text, u64 seed) noexcept
{
    u64 value = seed;
    for (const unsigned char c : text)
    {
        value ^= static_cast<u64>(c);
        value *= 1099511628211ULL;
    }
    return value;
}

[[nodiscard]] std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

[[nodiscard]] AssetKind KindFromExtension(const std::filesystem::path& path)
{
    const std::string extension = Lower(path.extension().string());
    if (extension == ".orbitmaterial") return AssetKind::Material;
    if (extension == ".orbitcomponent") return AssetKind::Component;
    if (extension == ".orbitdecal") return AssetKind::Decal;
    if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
        extension == ".tga" || extension == ".dds" || extension == ".ktx2" ||
        extension == ".exr") return AssetKind::Texture;
    if (extension == ".gltf" || extension == ".glb" || extension == ".fbx" ||
        extension == ".obj") return AssetKind::Mesh;
    return AssetKind::Unknown;
}

[[nodiscard]] std::string CanonicalImportSettings(
    const std::filesystem::path& source)
{
    std::filesystem::path sidecar =
        source;
    sidecar +=
        ".orbitimport.toml";

    if (!std::filesystem::is_regular_file(
            sidecar))
    {
        return {};
    }

    const toml::table settings =
        toml::parse_file(
            sidecar.string());

    std::ostringstream stream;
    stream << settings;
    return stream.str();
}

[[nodiscard]] std::filesystem::path ChannelPath(
    const toml::table& table, std::string_view key)
{
    const auto value = table[key].value<std::string>();
    return value.has_value() ? std::filesystem::path(*value) : std::filesystem::path{};
}
}

ContentService::ContentService(std::filesystem::path projectRoot)
    : projectRoot_(
          std::filesystem::weakly_canonical(
              std::move(projectRoot))),
      contentRoot_(
          projectRoot_ / "Content"),
      cache_(
          projectRoot_ /
          ".orbit" /
          "DerivedData"),
      pipeline_(
          importers_,
          cache_),
      thumbnails_(
          cache_)
{
    std::filesystem::create_directories(
        contentRoot_);

    RegisterBuiltinImporters(
        importers_);
}

void ContentService::Scan()
{
    std::unordered_map<AssetId, AssetRecord> next;
    std::unordered_map<std::string, AssetId> nextPaths;
    std::vector<ContentDiagnostic> diagnostics;

    if (std::filesystem::exists(contentRoot_))
    {
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(
                 contentRoot_,
                 std::filesystem::directory_options::
                     skip_permission_denied))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            try
            {
                AssetRecord record =
                    BuildRecord(entry.path());

                if (record.kind == AssetKind::Unknown)
                {
                    continue;
                }

                if (importers_.FindFor(
                        entry.path()) !=
                    nullptr)
                {
                    try
                    {
                        const auto imported =
                            pipeline_.Import(
                                projectRoot_,
                                entry.path(),
                                CanonicalImportSettings(
                                    entry.path()),
                                "source");

                        record.derivedKey =
                            imported.key;
                        record.derivedReady =
                            true;
                    }
                    catch (const std::exception&
                               exception)
                    {
                        diagnostics.push_back({
                            .sourcePath =
                                record.sourcePath,
                            .message =
                                std::string(
                                    "Derived import failed: ") +
                                exception.what()
                        });
                    }
                }

                const std::string key =
                    std::filesystem::relative(
                        entry.path(),
                        projectRoot_).
                        generic_string();

                nextPaths.insert_or_assign(
                    Lower(key),
                    record.id);
                next.insert_or_assign(
                    record.id,
                    std::move(record));
            }
            catch (const std::exception& exception)
            {
                diagnostics.push_back({
                    .sourcePath =
                        std::filesystem::relative(
                            entry.path(),
                            projectRoot_),
                    .message = exception.what()
                });
            }
        }
    }

    assets_ = std::move(next);
    pathIndex_ = std::move(nextPaths);
    diagnostics_ = std::move(diagnostics);
    ++revision_;
}

const AssetRecord* ContentService::Find(const AssetId id) const noexcept
{
    const auto found = assets_.find(id);
    return found == assets_.end() ? nullptr : &found->second;
}

const AssetRecord* ContentService::FindByPath(const std::filesystem::path& path) const noexcept
{
    try
    {
        const auto absolute = path.is_absolute() ? path : projectRoot_ / path;
        const std::string key = Lower(std::filesystem::relative(absolute, projectRoot_).generic_string());
        const auto indexed = pathIndex_.find(key);
        return indexed == pathIndex_.end() ? nullptr : Find(indexed->second);
    }
    catch (...)
    {
        return nullptr;
    }
}

std::vector<AssetRecord> ContentService::Search(
    const std::string_view query, const std::optional<AssetKind> kind) const
{
    const std::string needle = Lower(std::string(query));
    std::vector<AssetRecord> result;
    for (const auto& [id, asset] : assets_)
    {
        static_cast<void>(id);
        if (kind.has_value() && asset.kind != *kind) continue;
        std::string haystack = Lower(asset.name + " " + asset.sourcePath.generic_string());
        for (const auto& tag : asset.tags) haystack += " " + Lower(tag);
        if (needle.empty() || haystack.find(needle) != std::string::npos)
            result.push_back(asset);
    }
    std::ranges::sort(result, {}, &AssetRecord::name);
    return result;
}

std::vector<AssetRecord> ContentService::All() const
{
    return Search({});
}

const std::vector<ContentDiagnostic>&
ContentService::Diagnostics() const noexcept
{
    return diagnostics_;
}

u64 ContentService::Revision() const noexcept
{
    return revision_;
}

ImporterRegistry&
ContentService::Importers() noexcept
{
    return importers_;
}

const ImporterRegistry&
ContentService::Importers() const noexcept
{
    return importers_;
}

DerivedDataCache&
ContentService::Cache() noexcept
{
    return cache_;
}

const DerivedDataCache&
ContentService::Cache() const noexcept
{
    return cache_;
}

ThumbnailService&
ContentService::Thumbnails() noexcept
{
    return thumbnails_;
}

const ThumbnailService&
ContentService::Thumbnails() const noexcept
{
    return thumbnails_;
}

ThumbnailResult ContentService::GetThumbnail(
    const AssetId id,
    const u32 width,
    const u32 height)
{
    const AssetRecord* asset =
        Find(id);

    if (asset == nullptr)
    {
        throw std::invalid_argument(
            "Cannot thumbnail an unknown asset.");
    }

    return thumbnails_.Get({
        .sourceHash = asset->sourceHash,
        .category =
            std::string(
                AssetKindName(
                    asset->kind)),
        .label = asset->name,
        .width = width,
        .height = height
    });
}

ImportResult ContentService::ImportDerived(
    const AssetId id,
    std::string settings,
    std::string targetPlatform)
{
    const AssetRecord* asset =
        Find(id);

    if (asset == nullptr)
    {
        throw std::invalid_argument(
            "Cannot import an unknown asset.");
    }

    return pipeline_.Import(
        projectRoot_,
        projectRoot_ /
            asset->sourcePath,
        std::move(settings),
        std::move(targetPlatform));
}

AssetId ContentService::ImportFile(const std::filesystem::path& source)
{
    if (!std::filesystem::is_regular_file(source))
        throw std::invalid_argument("Content import source is not a regular file.");

    const auto destinationDirectory = contentRoot_ / "Imported";
    std::filesystem::create_directories(destinationDirectory);
    auto destination = destinationDirectory / source.filename();

    if (std::filesystem::exists(destination))
    {
        const std::string stem = source.stem().string();
        const std::string extension = source.extension().string();
        u32 suffix = 2;
        do
        {
            destination = destinationDirectory /
                (stem + "_" + std::to_string(suffix++) + extension);
        } while (std::filesystem::exists(destination));
    }

    std::filesystem::copy_file(source, destination);
    Scan();
    const AssetRecord* record = FindByPath(destination);
    if (record == nullptr)
        throw std::runtime_error("Imported file type is not supported by ContentService.");
    return record->id;
}

AssetRecord ContentService::BuildRecord(const std::filesystem::path& absolute) const
{
    AssetRecord result{
        .id = StableId(absolute),
        .kind = KindFromExtension(absolute),
        .name = absolute.stem().string(),
        .sourcePath =
            std::filesystem::relative(
                absolute,
                projectRoot_),
        .sourceHash =
            HashFile(
                absolute)
    };

    if (result.kind == AssetKind::Material)
    {
        const toml::table document = toml::parse_file(absolute.string());
        const toml::table* material = document["material"].as_table();
        if (material == nullptr)
            throw std::runtime_error("Material asset is missing [material]: " + absolute.string());

        if (const auto name = (*material)["name"].value<std::string>(); name.has_value())
            result.name = *name;

        MaterialChannels channels;
        channels.baseColor = ChannelPath(*material, "base_color");
        channels.normal = ChannelPath(*material, "normal");
        channels.roughness = ChannelPath(*material, "roughness");
        channels.metallic = ChannelPath(*material, "metallic");
        channels.ambientOcclusion = ChannelPath(*material, "ambient_occlusion");
        channels.emissive = ChannelPath(*material, "emissive");
        channels.roughnessFactor = (*material)["roughness_factor"].value_or(1.0);
        channels.metallicFactor = (*material)["metallic_factor"].value_or(0.0);
        result.material = std::move(channels);

        if (const toml::array* tags = (*material)["tags"].as_array())
            for (const auto& node : *tags)
                if (const auto tag = node.value<std::string>(); tag.has_value()) result.tags.push_back(*tag);
    }

    return result;
}

AssetId ContentService::StableId(const std::filesystem::path& absolute) const
{
    const std::string key = Lower(std::filesystem::relative(absolute, projectRoot_).generic_string());
    AssetId id{
        .high = Hash(key, 1469598103934665603ULL),
        .low = Hash(key, 1099511628211ULL ^ 0x9e3779b97f4a7c15ULL)
    };
    if (!id) id.low = 1;
    return id;
}

std::filesystem::path ContentService::NormalizeInsideProject(const std::filesystem::path& path) const
{
    const auto absolute = std::filesystem::weakly_canonical(path.is_absolute() ? path : projectRoot_ / path);
    const auto relative = absolute.lexically_relative(projectRoot_);
    if (relative.empty() || (!relative.empty() && *relative.begin() == ".."))
        throw std::invalid_argument("Content path escapes the project root.");
    return absolute;
}

std::string_view AssetKindName(const AssetKind kind) noexcept
{
    switch (kind)
    {
    case AssetKind::Texture: return "Texture";
    case AssetKind::Material: return "Material";
    case AssetKind::Decal: return "Decal";
    case AssetKind::Component: return "Component";
    case AssetKind::Mesh: return "Mesh";
    case AssetKind::Unknown: return "Unknown";
    }
    return "Unknown";
}
} // namespace orbit::content
