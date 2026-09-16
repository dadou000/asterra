#include <orbit/content/AssetPipeline.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace orbit::content
{
namespace
{
constexpr std::string_view kManifestName =
    "manifest.toml";

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

[[nodiscard]] std::string NormalizeExtension(
    std::string extension)
{
    extension = Lower(
        std::move(extension));

    if (extension.empty() ||
        extension.front() != '.')
    {
        throw std::invalid_argument(
            "Importer extension must begin with '.'.");
    }

    return extension;
}

[[nodiscard]] std::vector<std::byte>
StringBytes(
    const std::string_view text)
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

[[nodiscard]] std::string
ReadManifestText(
    DerivedDataCache& cache,
    const ContentHash& key)
{
    const auto bytes =
        cache.Read(
            key,
            kManifestName);

    if (!bytes.has_value())
    {
        return {};
    }

    return std::string(
        reinterpret_cast<const char*>(
            bytes->data()),
        bytes->size());
}

[[nodiscard]] std::filesystem::path
ResolveDependency(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& sourceDirectory,
    const std::filesystem::path& dependency)
{
    const auto candidate =
        dependency.is_absolute()
            ? dependency
            : sourceDirectory /
                dependency;

    const auto absolute =
        std::filesystem::weakly_canonical(
            candidate);

    const auto relative =
        absolute.lexically_relative(
            projectRoot);

    if (relative.empty() ||
        *relative.begin() == "..")
    {
        throw std::invalid_argument(
            "Importer dependency escapes the project root: " +
            dependency.string());
    }

    if (!std::filesystem::is_regular_file(
            absolute))
    {
        throw std::runtime_error(
            "Importer dependency does not exist: " +
            absolute.string());
    }

    return absolute;
}

[[nodiscard]] bool TryUseCachedImport(
    DerivedDataCache& cache,
    const ContentHash& key,
    const std::filesystem::path& projectRoot,
    ImportResult& result)
{
    const std::string manifestText =
        ReadManifestText(
            cache,
            key);

    if (manifestText.empty())
    {
        return false;
    }

    toml::table manifest;

    try
    {
        manifest =
            toml::parse(
                manifestText);
    }
    catch (const toml::parse_error&)
    {
        return false;
    }

    const toml::array* artifacts =
        manifest["artifacts"].as_array();

    if (artifacts == nullptr)
    {
        return false;
    }

    result.artifacts.clear();

    for (const auto& node : *artifacts)
    {
        const auto name =
            node.value<std::string>();

        if (!name.has_value() ||
            !cache.Contains(
                key,
                *name))
        {
            return false;
        }

        result.artifacts.push_back({
            .name = *name,
            .path =
                cache.ArtifactPath(
                    key,
                    *name)
        });
    }

    result.dependencies.clear();

    if (const toml::array* dependencies =
            manifest["dependencies"].
                as_array();
        dependencies != nullptr)
    {
        for (const auto& node :
             *dependencies)
        {
            const toml::table* item =
                node.as_table();

            if (item == nullptr)
            {
                return false;
            }

            const auto path =
                (*item)["path"].
                    value<std::string>();
            const auto hash =
                (*item)["sha256"].
                    value<std::string>();

            if (!path.has_value() ||
                !hash.has_value())
            {
                return false;
            }

            const auto absolute =
                std::filesystem::weakly_canonical(
                    projectRoot /
                    std::filesystem::path(
                        *path));

            if (!std::filesystem::is_regular_file(
                    absolute) ||
                HashFile(absolute).ToHex() !=
                    *hash)
            {
                return false;
            }

            result.dependencies.push_back(
                std::filesystem::relative(
                    absolute,
                    projectRoot));
        }
    }

    result.cacheHit = true;
    return true;
}

void StoreManifest(
    DerivedDataCache& cache,
    const ImportResult& result,
    const std::filesystem::path& projectRoot)
{
    toml::table manifest;

    manifest.insert(
        "importer",
        result.importerId);
    manifest.insert(
        "importer_version",
        static_cast<i64>(
            result.importerVersion));
    manifest.insert(
        "source_sha256",
        result.sourceHash.ToHex());

    toml::array artifacts;

    for (const auto& artifact :
         result.artifacts)
    {
        artifacts.push_back(
            artifact.name);
    }

    manifest.insert(
        "artifacts",
        std::move(artifacts));

    toml::array dependencies;

    for (const auto& relative :
         result.dependencies)
    {
        const auto absolute =
            projectRoot /
            relative;

        toml::table item;
        item.insert(
            "path",
            relative.generic_string());
        item.insert(
            "sha256",
            HashFile(
                absolute).
                ToHex());

        dependencies.push_back(
            std::move(item));
    }

    manifest.insert(
        "dependencies",
        std::move(dependencies));

    std::ostringstream stream;
    stream << manifest;

    const std::string text =
        stream.str();

    static_cast<void>(
        cache.Store(
            result.key,
            kManifestName,
            StringBytes(text)));
}
} // namespace

void RegisterBuiltinImporters(
    ImporterRegistry& registry)
{
    registry.Register({
        .id = "orbit.material",
        .version = 1,
        .extensions = {
            ".orbitmaterial"
        },
        .import =
            [](
                const ImportRequest& request)
            {
                const toml::table document =
                    toml::parse_file(
                        request.sourcePath.
                            string());

                const toml::table* material =
                    document["material"].
                        as_table();

                if (material == nullptr)
                {
                    throw std::runtime_error(
                        "Material source is missing [material].");
                }

                std::vector<std::filesystem::path>
                    dependencies;

                constexpr std::array<
                    std::string_view,
                    6>
                    kTextureChannels{
                        "base_color",
                        "normal",
                        "roughness",
                        "metallic",
                        "ambient_occlusion",
                        "emissive"
                    };

                for (const auto channel :
                     kTextureChannels)
                {
                    if (const auto path =
                            (*material)[channel].
                                value<std::string>();
                        path.has_value() &&
                        !path->empty())
                    {
                        dependencies.emplace_back(
                            *path);
                    }
                }

                std::ostringstream stream;
                stream << document;

                const std::string normalized =
                    stream.str();

                return ImportOutput{
                    .artifacts = {
                        {
                            .name =
                                "material.toml",
                            .bytes =
                                StringBytes(
                                    normalized)
                        }
                    },
                    .dependencies =
                        std::move(
                            dependencies)
                };
            }
    });
}

void ImporterRegistry::Register(
    ImporterDescriptor descriptor)
{
    if (descriptor.id.empty() ||
        descriptor.version == 0 ||
        descriptor.extensions.empty() ||
        !descriptor.import)
    {
        throw std::invalid_argument(
            "Importer requires id, non-zero version, extensions and callback.");
    }

    if (std::ranges::any_of(
            importers_,
            [&descriptor](
                const ImporterDescriptor& existing)
            {
                return existing.id ==
                    descriptor.id;
            }))
    {
        throw std::invalid_argument(
            "Importer id is already registered: " +
            descriptor.id);
    }

    std::set<std::string>
        normalizedExtensions;

    for (std::string& extension :
         descriptor.extensions)
    {
        extension =
            NormalizeExtension(
                std::move(extension));

        if (!normalizedExtensions.insert(
                extension).
                second)
        {
            throw std::invalid_argument(
                "Importer declares duplicate extension: " +
                extension);
        }

        for (const auto& existing :
             importers_)
        {
            if (std::ranges::find(
                    existing.extensions,
                    extension) !=
                existing.extensions.end())
            {
                throw std::invalid_argument(
                    "Importer extension already has an owner: " +
                    extension);
            }
        }
    }

    importers_.push_back(
        std::move(descriptor));
}

const ImporterDescriptor*
ImporterRegistry::FindFor(
    const std::filesystem::path& source) const
{
    const std::string extension =
        Lower(
            source.extension().
                string());

    const auto found =
        std::ranges::find_if(
            importers_,
            [&extension](
                const ImporterDescriptor& importer)
            {
                return std::ranges::find(
                    importer.extensions,
                    extension) !=
                    importer.extensions.end();
            });

    return found == importers_.end()
        ? nullptr
        : &*found;
}

std::vector<ImporterCatalogEntry>
ImporterRegistry::Catalog() const
{
    std::vector<ImporterCatalogEntry>
        result;

    result.reserve(
        importers_.size());

    for (const auto& importer :
         importers_)
    {
        result.push_back({
            .id = importer.id,
            .version = importer.version,
            .extensions =
                importer.extensions
        });
    }

    std::ranges::sort(
        result,
        {},
        &ImporterCatalogEntry::id);

    return result;
}

ContentHash BuildDerivedDataKey(
    const ContentHash& sourceHash,
    const std::string_view importerId,
    const u32 importerVersion,
    const std::string_view settings,
    const std::string_view targetPlatform)
{
    std::string canonical =
        "orbit-ddc-v1\n";

    canonical +=
        sourceHash.ToHex();
    canonical.push_back('\n');
    canonical.append(
        importerId);
    canonical.push_back('\n');
    canonical +=
        std::to_string(
            importerVersion);
    canonical.push_back('\n');
    canonical.append(
        targetPlatform);
    canonical.push_back('\n');
    canonical.append(
        settings);

    return HashString(
        canonical);
}

AssetPipeline::AssetPipeline(
    const ImporterRegistry& importers,
    DerivedDataCache& cache)
    : importers_(importers),
      cache_(cache)
{
}

ImportResult AssetPipeline::Import(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& source,
    std::string settings,
    std::string targetPlatform,
    const std::optional<ContentHash> knownSourceHash)
{
    const auto normalizedRoot =
        std::filesystem::weakly_canonical(
            projectRoot);

    const auto normalizedSource =
        std::filesystem::weakly_canonical(
            source.is_absolute()
                ? source
                : normalizedRoot /
                    source);

    const auto relativeSource =
        normalizedSource.lexically_relative(
            normalizedRoot);

    if (relativeSource.empty() ||
        *relativeSource.begin() == ".." ||
        !std::filesystem::is_regular_file(
            normalizedSource))
    {
        throw std::invalid_argument(
            "Import source must be a project file.");
    }

    const ImporterDescriptor* importer =
        importers_.FindFor(
            normalizedSource);

    if (importer == nullptr)
    {
        throw std::invalid_argument(
            "No importer is registered for source: " +
            normalizedSource.string());
    }

    const ContentHash sourceHash =
        knownSourceHash.has_value()
            ? *knownSourceHash
            : HashFile(
                  normalizedSource);

    ImportResult result{
        .key =
            BuildDerivedDataKey(
                sourceHash,
                importer->id,
                importer->version,
                settings,
                targetPlatform),
        .sourceHash = sourceHash,
        .importerId = importer->id,
        .importerVersion =
            importer->version
    };

    if (TryUseCachedImport(
            cache_,
            result.key,
            normalizedRoot,
            result))
    {
        return result;
    }

    // A missing/invalid manifest or changed dependency means this key's
    // previously derived products are stale or incomplete. Remove the whole
    // entry before regeneration so immutable Store() cannot preserve an old
    // artifact under the same primary-source key.
    cache_.Remove(
        result.key);

    result.artifacts.clear();
    result.dependencies.clear();

    ImportOutput output =
        importer->import({
            .sourcePath =
                normalizedSource,
            .projectRelativePath =
                relativeSource,
            .settings =
                std::move(settings),
            .targetPlatform =
                std::move(targetPlatform)
        });

    if (output.artifacts.empty())
    {
        throw std::runtime_error(
            "Importer produced no derived artifacts: " +
            importer->id);
    }

    std::set<std::string>
        artifactNames;

    for (const auto& artifact :
         output.artifacts)
    {
        if (artifact.name ==
                kManifestName ||
            !artifactNames.insert(
                artifact.name).
                second)
        {
            throw std::runtime_error(
                "Importer produced an invalid or duplicate artifact name: " +
                artifact.name);
        }

        const auto path =
            cache_.Store(
                result.key,
                artifact.name,
                artifact.bytes);

        result.artifacts.push_back({
            .name = artifact.name,
            .path = path
        });
    }

    const auto sourceDirectory =
        normalizedSource.parent_path();

    std::set<std::string>
        dependencyKeys;

    for (const auto& dependency :
         output.dependencies)
    {
        const auto absolute =
            ResolveDependency(
                normalizedRoot,
                sourceDirectory,
                dependency);

        const auto relative =
            std::filesystem::relative(
                absolute,
                normalizedRoot);

        if (relative ==
            relativeSource)
        {
            continue;
        }

        const std::string key =
            relative.generic_string();

        if (dependencyKeys.insert(
                key).
                second)
        {
            result.dependencies.push_back(
                relative);
        }
    }

    StoreManifest(
        cache_,
        result,
        normalizedRoot);

    result.cacheHit = false;
    return result;
}
} // namespace orbit::content
