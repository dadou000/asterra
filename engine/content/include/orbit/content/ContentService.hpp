#pragma once

#include <orbit/content/AssetPipeline.hpp>
#include <orbit/content/ThumbnailService.hpp>
#include <orbit/core/StrongId.hpp>
#include <orbit/core/Types.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace orbit::content
{
struct AssetIdTag;
using AssetId = core::StrongId<AssetIdTag>;

enum class AssetKind : u8
{
    Texture,
    Material,
    MaterialInstance,
    Decal,
    Component,
    Mesh,
    PathProfile,
    Shader,
    Unknown
};

struct MaterialEmission
{
    std::array<f64, 3> colorLinear{1.0, 1.0, 1.0};
    f64 luminanceNits{0.0};
    bool contributesToGi{true};
    f64 giScale{1.0};
};

struct MaterialChannels
{
    std::filesystem::path baseColor;
    std::filesystem::path normal;
    std::filesystem::path roughness;
    std::filesystem::path metallic;
    std::filesystem::path ambientOcclusion;
    std::filesystem::path emissive;
    f64 roughnessFactor{1.0};
    f64 metallicFactor{0.0};
    MaterialEmission emission{};
};

struct MaterialInstanceData
{
    std::filesystem::path parent;
    std::optional<f64> roughnessFactor;
    std::optional<f64> metallicFactor;
    std::optional<std::array<f64, 3>> emissionColorLinear;
    std::optional<f64> emissionLuminanceNits;
    std::optional<bool> emissionContributesToGi;
    std::optional<f64> emissionGiScale;
};

struct DecalData
{
    std::filesystem::path texture;
    f64 widthMeters{1.0};
    f64 heightMeters{1.0};
    f64 opacity{1.0};
};

enum class ShaderAssetStage : u8
{
    Vertex,
    Pixel,
    Compute
};

struct ShaderAssetData
{
    ShaderAssetStage stage{
        ShaderAssetStage::Vertex};
    std::string entryPoint{"main"};
};

struct AssetRecord
{
    AssetId id{};
    AssetKind kind{AssetKind::Unknown};
    std::string name;
    std::filesystem::path sourcePath;
    ContentHash sourceHash{};
    std::optional<ContentHash> derivedKey;
    bool derivedReady{false};
    std::vector<std::filesystem::path>
        dependencyPaths;
    std::vector<AssetId> dependencies;
    std::vector<std::string> tags;
    std::optional<MaterialChannels> material;
    std::optional<MaterialInstanceData>
        materialInstance;
    std::optional<DecalData> decal;
    std::optional<ShaderAssetData>
        shader;
};

struct ContentDiagnostic
{
    std::filesystem::path sourcePath;
    std::string message;
};

class ContentService
{
public:
    explicit ContentService(std::filesystem::path projectRoot);

    ContentService(const ContentService&) = delete;
    ContentService& operator=(const ContentService&) = delete;
    ContentService(ContentService&&) = delete;
    ContentService& operator=(ContentService&&) = delete;

    // Rebuilds the in-memory index from project Content mounts. IDs are
    // derived from normalized project-relative paths and remain stable.
    void Scan();

    [[nodiscard]] const AssetRecord* Find(AssetId id) const noexcept;
    [[nodiscard]] const AssetRecord* FindByPath(const std::filesystem::path& path) const noexcept;
    [[nodiscard]] std::vector<AssetRecord> Search(std::string_view query, std::optional<AssetKind> kind = std::nullopt) const;
    [[nodiscard]] std::vector<AssetRecord> All() const;
    [[nodiscard]] std::vector<AssetId> Dependencies(
        AssetId id) const;
    [[nodiscard]] std::vector<AssetId> Dependents(
        AssetId id) const;
    [[nodiscard]] const std::vector<ContentDiagnostic>& Diagnostics() const noexcept;
    [[nodiscard]] u64 Revision() const noexcept;

    [[nodiscard]] ImporterRegistry& Importers() noexcept;
    [[nodiscard]] const ImporterRegistry& Importers() const noexcept;
    [[nodiscard]] DerivedDataCache& Cache() noexcept;
    [[nodiscard]] const DerivedDataCache& Cache() const noexcept;
    [[nodiscard]] ThumbnailService& Thumbnails() noexcept;
    [[nodiscard]] const ThumbnailService& Thumbnails() const noexcept;

    [[nodiscard]] ThumbnailResult GetThumbnail(
        AssetId id,
        u32 width = 96,
        u32 height = 96);

    // Runs the registered importer for an indexed source asset using the
    // shared project DDC. This does not replace or mutate project authority.
    [[nodiscard]] ImportResult ImportDerived(
        AssetId id,
        std::string settings = {},
        std::string targetPlatform = "source");

    // Target-platform cook path used by BuildService. It preserves the
    // asset's canonical .orbitimport.toml settings so editor imports and
    // headless cooks share exactly the same DDC key semantics.
    [[nodiscard]] ImportResult CookDerived(
        AssetId id,
        std::string targetPlatform);

    // Imports a source file into Content/Imported without temporary staging
    // formats. The copied source becomes the canonical project asset.
    [[nodiscard]] AssetId ImportFile(const std::filesystem::path& source);

    // Detects common PBR channel maps in a source directory, copies the
    // recognized source maps into project Content, writes a reusable
    // .orbitmaterial authority file, rescans the registry, and returns its ID.
    // Ambiguous duplicate channels are rejected rather than guessed.
    [[nodiscard]] AssetId ImportPbrSet(
        const std::filesystem::path& sourceDirectory,
        std::string materialName = {});

    // Creates a reusable material-instance authority asset that references a
    // base material and stores only overrides. The base remains immutable.
    [[nodiscard]] AssetId CreateMaterialInstance(
        AssetId baseMaterial,
        std::string instanceName = {});

    void SetMaterialEmission(
        AssetId materialAsset,
        const MaterialEmission& emission);

    // Creates a persistent .orbitdecal authority asset from an indexed texture.
    // The decal owns semantic size/opacity metadata and references the texture
    // through the ordinary asset dependency graph/DDC pipeline.
    [[nodiscard]] AssetId CreateDecal(
        AssetId textureAsset,
        std::string decalName = {},
        f64 widthMeters = 1.0,
        f64 heightMeters = 1.0,
        f64 opacity = 1.0);

private:
    [[nodiscard]] AssetRecord BuildRecord(const std::filesystem::path& absolute) const;
    [[nodiscard]] AssetId StableId(const std::filesystem::path& absolute) const;
    [[nodiscard]] std::filesystem::path NormalizeInsideProject(const std::filesystem::path& path) const;

    std::filesystem::path projectRoot_;
    std::filesystem::path contentRoot_;
    ImporterRegistry importers_;
    DerivedDataCache cache_;
    AssetPipeline pipeline_;
    ThumbnailService thumbnails_;
    std::unordered_map<AssetId, AssetRecord> assets_;
    std::unordered_map<std::string, AssetId> pathIndex_;
    std::unordered_map<AssetId, std::vector<AssetId>>
        dependents_;
    std::vector<ContentDiagnostic> diagnostics_;
    u64 revision_{0};
};

[[nodiscard]] std::string_view AssetKindName(AssetKind kind) noexcept;
} // namespace orbit::content
