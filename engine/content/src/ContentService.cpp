#include <orbit/content/ContentService.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
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
    if (extension == ".orbitmaterialinstance") return AssetKind::MaterialInstance;
    if (extension == ".orbitcomponent") return AssetKind::Component;
    if (extension == ".orbitdecal") return AssetKind::Decal;
    if (extension == ".orbitpathprofile") return AssetKind::PathProfile;
    if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
        extension == ".bmp" || extension == ".tga" || extension == ".dds" ||
        extension == ".ktx2" || extension == ".exr") return AssetKind::Texture;
    if (extension == ".gltf" || extension == ".glb" || extension == ".fbx" ||
        extension == ".obj") return AssetKind::Mesh;
    if (extension == ".hlsl") return AssetKind::Shader;
    if (extension == ".cube") return AssetKind::ColorLut;
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

[[nodiscard]] std::optional<std::array<f64, 3>> Color3(
    const toml::table& table,
    const std::string_view key)
{
    const auto* array =
        table[key].as_array();

    if (array == nullptr)
    {
        return std::nullopt;
    }

    if (array->size() != 3U)
    {
        throw std::runtime_error(
            std::string(key) +
            " must contain exactly three numeric values.");
    }

    std::array<f64, 3> result{};

    for (std::size_t index = 0U;
         index < result.size();
         ++index)
    {
        const auto value =
            (*array)[index].value<f64>();

        if (!value.has_value() ||
            !std::isfinite(*value) ||
            *value < 0.0)
        {
            throw std::runtime_error(
                std::string(key) +
                " values must be finite and non-negative.");
        }

        result[index] = *value;
    }

    return result;
}

void ValidateEmission(
    const MaterialEmission& emission)
{
    if (!std::isfinite(
            emission.luminanceNits) ||
        emission.luminanceNits < 0.0 ||
        !std::isfinite(emission.giScale) ||
        emission.giScale < 0.0)
    {
        throw std::runtime_error(
            "Material emission luminance and GI scale must be finite and non-negative.");
    }

    for (const f64 channel :
         emission.colorLinear)
    {
        if (!std::isfinite(channel) ||
            channel < 0.0)
        {
            throw std::runtime_error(
                "Material emission color must be finite and non-negative.");
        }
    }
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
    RegisterDecalImporter(
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
                                "source",
                                record.sourceHash);

                        record.derivedKey =
                            imported.key;
                        record.derivedReady =
                            true;
                        record.dependencyPaths =
                            imported.dependencies;
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

    std::unordered_map<
        AssetId,
        std::vector<AssetId>>
        nextDependents;

    for (auto& [assetId, record] :
         next)
    {
        for (const auto& dependencyPath :
             record.dependencyPaths)
        {
            const auto found =
                nextPaths.find(
                    Lower(
                        dependencyPath.
                            generic_string()));

            if (found ==
                nextPaths.end())
            {
                continue;
            }

            record.dependencies.push_back(
                found->second);

            nextDependents[
                found->second].
                push_back(
                    assetId);
        }

        std::ranges::sort(
            record.dependencies);

        record.dependencies.erase(
            std::unique(
                record.dependencies.begin(),
                record.dependencies.end()),
            record.dependencies.end());
    }

    for (auto& [dependency, users] :
         nextDependents)
    {
        static_cast<void>(
            dependency);

        std::ranges::sort(
            users);

        users.erase(
            std::unique(
                users.begin(),
                users.end()),
            users.end());
    }

    assets_ = std::move(next);
    pathIndex_ = std::move(nextPaths);
    dependents_ =
        std::move(nextDependents);
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

std::filesystem::path ContentService::AbsolutePath(
    const AssetId id) const
{
    const AssetRecord* asset =
        Find(id);

    if (asset == nullptr)
    {
        throw std::invalid_argument(
            "Cannot resolve the path of an unknown asset.");
    }

    return projectRoot_ /
        asset->sourcePath;
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

std::vector<AssetId> ContentService::Dependencies(
    const AssetId id) const
{
    const AssetRecord* asset =
        Find(id);

    return asset != nullptr
        ? asset->dependencies
        : std::vector<AssetId>{};
}

std::vector<AssetId> ContentService::Dependents(
    const AssetId id) const
{
    const auto found =
        dependents_.find(
            id);

    return found != dependents_.end()
        ? found->second
        : std::vector<AssetId>{};
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
        std::move(targetPlatform),
        asset->sourceHash);
}

ImportResult ContentService::CookDerived(
    const AssetId id,
    std::string targetPlatform)
{
    const AssetRecord* asset =
        Find(id);

    if (asset == nullptr)
    {
        throw std::invalid_argument(
            "Cannot cook an unknown asset.");
    }

    const std::filesystem::path source =
        projectRoot_ /
        asset->sourcePath;

    return pipeline_.Import(
        projectRoot_,
        source,
        CanonicalImportSettings(source),
        std::move(targetPlatform),
        asset->sourceHash);
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
    const AssetKind kind =
        KindFromExtension(
            absolute);

    AssetRecord result{
        .id = StableId(absolute),
        .kind = kind,
        .name = absolute.stem().string(),
        .sourcePath =
            std::filesystem::relative(
                absolute,
                projectRoot_)
    };

    if (kind == AssetKind::Unknown)
    {
        return result;
    }

    result.sourceHash =
        HashFile(
            absolute);

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

        if (const auto color =
                Color3(*material, "emission_color_linear");
            color.has_value())
        {
            channels.emission.colorLinear =
                *color;
        }

        channels.emission.luminanceNits =
            (*material)["emission_luminance_nits"].
                value_or(0.0);

        channels.emission.contributesToGi =
            (*material)["emission_gi_enabled"].
                value_or(true);

        channels.emission.giScale =
            (*material)["emission_gi_scale"].
                value_or(1.0);

        ValidateEmission(
            channels.emission);

        result.material = std::move(channels);

        if (const toml::array* tags = (*material)["tags"].as_array())
            for (const auto& node : *tags)
                if (const auto tag = node.value<std::string>(); tag.has_value()) result.tags.push_back(*tag);
    }
    else if (result.kind == AssetKind::MaterialInstance)
    {
        const toml::table document =
            toml::parse_file(
                absolute.string());

        const toml::table* instance =
            document["material_instance"].
                as_table();

        if (instance == nullptr)
        {
            throw std::runtime_error(
                "Material instance is missing [material_instance]: " +
                absolute.string());
        }

        const auto parent =
            (*instance)["parent"].
                value<std::string>();

        if (!parent.has_value() ||
            parent->empty())
        {
            throw std::runtime_error(
                "Material instance requires a parent material.");
        }

        if (const auto name =
                (*instance)["name"].
                    value<std::string>();
            name.has_value())
        {
            result.name = *name;
        }

        MaterialInstanceData data{
            .parent =
                std::filesystem::path(
                    *parent),
            .roughnessFactor =
                (*instance)[
                    "roughness_factor"].
                    value<f64>(),
            .metallicFactor =
                (*instance)[
                    "metallic_factor"].
                    value<f64>(),
            .emissionColorLinear =
                Color3(
                    *instance,
                    "emission_color_linear"),
            .emissionLuminanceNits =
                (*instance)[
                    "emission_luminance_nits"].
                    value<f64>(),
            .emissionContributesToGi =
                (*instance)[
                    "emission_gi_enabled"].
                    value<bool>(),
            .emissionGiScale =
                (*instance)[
                    "emission_gi_scale"].
                    value<f64>()
        };

        if (data.emissionLuminanceNits.has_value() &&
            (!std::isfinite(*data.emissionLuminanceNits) ||
             *data.emissionLuminanceNits < 0.0))
        {
            throw std::runtime_error(
                "Material-instance emission luminance must be finite and non-negative.");
        }

        if (data.emissionGiScale.has_value() &&
            (!std::isfinite(*data.emissionGiScale) ||
             *data.emissionGiScale < 0.0))
        {
            throw std::runtime_error(
                "Material-instance emission GI scale must be finite and non-negative.");
        }

        result.materialInstance =
            std::move(data);

        if (const toml::array* tags =
                (*instance)["tags"].
                    as_array())
        {
            for (const auto& node :
                 *tags)
            {
                if (const auto tag =
                        node.value<std::string>();
                    tag.has_value())
                {
                    result.tags.push_back(
                        *tag);
                }
            }
        }
    }
    else if (result.kind == AssetKind::Decal)
    {
        const toml::table document =
            toml::parse_file(
                absolute.string());

        const toml::table* decal =
            document["decal"].as_table();

        if (decal == nullptr)
        {
            throw std::runtime_error(
                "Decal asset is missing [decal]: " +
                absolute.string());
        }

        const auto texture =
            (*decal)["texture"].value<std::string>();

        if (!texture.has_value() ||
            texture->empty())
        {
            throw std::runtime_error(
                "Decal asset requires a texture dependency.");
        }

        if (const auto name =
                (*decal)["name"].value<std::string>();
            name.has_value() && !name->empty())
        {
            result.name = *name;
        }

        DecalData data{
            .texture = std::filesystem::path(*texture),
            .widthMeters =
                (*decal)["width_meters"].value_or(1.0),
            .heightMeters =
                (*decal)["height_meters"].value_or(1.0),
            .opacity =
                (*decal)["opacity"].value_or(1.0)
        };

        if (!(data.widthMeters > 0.0) ||
            !(data.heightMeters > 0.0) ||
            data.opacity < 0.0 ||
            data.opacity > 1.0)
        {
            throw std::runtime_error(
                "Decal dimensions must be positive and opacity must be in [0, 1].");
        }

        result.decal = std::move(data);
        result.tags.push_back("decal");

        if (const toml::array* tags =
                (*decal)["tags"].as_array())
        {
            for (const auto& node : *tags)
            {
                if (const auto tag = node.value<std::string>();
                    tag.has_value() && *tag != "decal")
                {
                    result.tags.push_back(*tag);
                }
            }
        }
    }
    else if (result.kind == AssetKind::PathProfile)
    {
        const toml::table document =
            toml::parse_file(
                absolute.string());

        const toml::table* profile =
            document["path_profile"].
                as_table();

        if (profile == nullptr)
        {
            throw std::runtime_error(
                "Path profile asset is missing [path_profile]: " +
                absolute.string());
        }

        const auto name =
            (*profile)["name"].
                value<std::string>();
        const auto profileKind =
            (*profile)["kind"].
                value<std::string>();

        if (!name.has_value() ||
            name->empty() ||
            !profileKind.has_value() ||
            profileKind->empty())
        {
            throw std::runtime_error(
                "Path profile requires non-empty name and kind.");
        }

        result.name = *name;
        result.tags.push_back("path");
        result.tags.push_back(
            Lower(*profileKind));
    }
    else if (result.kind == AssetKind::Shader)
    {
        std::filesystem::path sidecar =
            absolute;
        sidecar +=
            ".orbitshader.toml";

        if (!std::filesystem::
                is_regular_file(
                    sidecar))
        {
            throw std::runtime_error(
                "HLSL shader requires a .orbitshader.toml sidecar: " +
                absolute.string());
        }

        const toml::table document =
            toml::parse_file(
                sidecar.string());

        const toml::table* shader =
            document["shader"].
                as_table();

        if (shader == nullptr)
        {
            throw std::runtime_error(
                "Shader sidecar is missing [shader]: " +
                sidecar.string());
        }

        const auto stage =
            (*shader)["stage"].
                value<std::string>();
        const auto entry =
            (*shader)["entry"].
                value<std::string>();

        if (!stage.has_value() ||
            stage->empty() ||
            !entry.has_value() ||
            entry->empty())
        {
            throw std::runtime_error(
                "Shader sidecar requires non-empty stage and entry.");
        }

        ShaderAssetStage parsedStage{};

        if (*stage == "vertex")
        {
            parsedStage =
                ShaderAssetStage::Vertex;
        }
        else if (*stage == "pixel")
        {
            parsedStage =
                ShaderAssetStage::Pixel;
        }
        else if (*stage == "compute")
        {
            parsedStage =
                ShaderAssetStage::Compute;
        }
        else
        {
            throw std::runtime_error(
                "Shader stage must be vertex, pixel, or compute.");
        }

        result.shader =
            ShaderAssetData{
                .stage = parsedStage,
                .entryPoint = *entry
            };

        result.tags.push_back(
            "shader");
        result.tags.push_back(
            *stage);

        result.sourceHash =
            HashString(
                result.sourceHash.ToHex() +
                "|" +
                HashFile(sidecar).ToHex());
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
    case AssetKind::MaterialInstance: return "Material Instance";
    case AssetKind::Decal: return "Decal";
    case AssetKind::Component: return "Component";
    case AssetKind::Mesh: return "Mesh";
    case AssetKind::PathProfile: return "Path Profile";
    case AssetKind::Shader: return "Shader";
    case AssetKind::ColorLut: return "Color LUT";
    case AssetKind::Unknown: return "Unknown";
    }
    return "Unknown";
}
} // namespace orbit::content
