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
    ColorLut,
    // Shading-tab assets. A ShadingShader is a `*.shade.hlsl` file holding the
    // Orbit shading contract's `Shade()` function (see docs/ORBIT_SHADING.md);
    // it is not a standalone HLSL stage, so it needs no `.orbitshader.toml`
    // sidecar and is not cooked as a Shader. A ShaderMaterial binds a
    // ShadingShader to named parameter values.
    ShadingShader,
    ShaderMaterial,
    Unknown
};

struct ShaderMaterialParameter
{
    std::string name;
    // One to four scalar components (float, vec2, vec3 or vec4).
    std::vector<f64> values;
};

struct ShaderMaterialTexture
{
    std::string name;
    // Project-relative path of the image (e.g. "Content/Textures/Rock.jpg").
    std::string path;
};

struct ShaderMaterialData
{
    // Project-relative or material-folder-relative path of the shader.
    std::filesystem::path shader;
    std::vector<ShaderMaterialParameter> parameters;
    std::vector<ShaderMaterialTexture> textures;
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
    std::optional<ShaderMaterialData>
        shaderMaterial;
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
    [[nodiscard]] std::filesystem::path AbsolutePath(AssetId id) const;
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

    [[nodiscard]] MaterialEmission ResolveMaterialEmission(
        AssetId materialAsset) const;

    // Assigns or clears the emissive texture channel on a base material.
    // Material instances inherit the texture from their parent in V0.0.7.
    void SetMaterialEmissiveTexture(
        AssetId materialAsset,
        std::filesystem::path texturePath);

    // Creates a persistent .orbitdecal authority asset from an indexed texture.
    // The decal owns semantic size/opacity metadata and references the texture
    // through the ordinary asset dependency graph/DDC pipeline.
    [[nodiscard]] AssetId CreateDecal(
        AssetId textureAsset,
        std::string decalName = {},
        f64 widthMeters = 1.0,
        f64 heightMeters = 1.0,
        f64 opacity = 1.0);

    // ---- Content organisation -------------------------------------------
    //
    // Every path below is project-relative and must lie inside the Content
    // mount (for example `Content/Shaders/Lunar.shade.hlsl`). Folders are real
    // directories, so an external file manager or editor sees the same tree.
    // Each mutating call rescans, so it is reflected by Find/Search/Revision
    // immediately and by the hot-iteration refresh with no restart.
    //
    // Moving or renaming an asset changes its path-derived AssetId; references
    // stored as paths (for example a shader material's `shader`) are not
    // rewritten.

    // All folders under Content (empty ones included), project-relative,
    // sorted, excluding the Content root itself.
    [[nodiscard]] std::vector<std::filesystem::path> Folders() const;

    void CreateFolder(const std::filesystem::path& folder);

    // Renames a file or folder in place. `newName` is a single path component.
    // Returns the new project-relative path.
    std::filesystem::path RenameEntry(
        const std::filesystem::path& entry,
        std::string_view newName);

    // Moves a file or folder into an existing destination folder. Refuses to
    // overwrite or to move a folder into itself. Returns the new path.
    std::filesystem::path MoveEntry(
        const std::filesystem::path& entry,
        const std::filesystem::path& destinationFolder);

    // Reversible removal: the entry moves to `.orbit/Trash/<stamp>/...` inside
    // the project instead of being deleted. Returns the trashed location.
    std::filesystem::path TrashEntry(
        const std::filesystem::path& entry);

    // UTF-8 text files inside Content (shader sources and small TOML assets).
    [[nodiscard]] std::string ReadText(
        const std::filesystem::path& file) const;
    // Creates or replaces the file atomically (temp file + rename) so a
    // hot-reload watcher never observes a half-written shader.
    void WriteText(
        const std::filesystem::path& file,
        std::string_view text);

private:
    class HotIterationRegistration
    {
    public:
        explicit HotIterationRegistration(ContentService* owner);
        ~HotIterationRegistration();

        HotIterationRegistration(const HotIterationRegistration&) = delete;
        HotIterationRegistration& operator=(const HotIterationRegistration&) = delete;

    private:
        ContentService* owner_{nullptr};
        u64 contentHandler_{0};
        u64 shaderHandler_{0};
    };

    // Validates a project-relative Content path and returns its absolute form.
    // Throws when it escapes the Content mount or (mustExist) is missing.
    [[nodiscard]] std::filesystem::path ResolveContentEntry(
        const std::filesystem::path& entry,
        bool mustExist) const;

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

    // Constructed last so projectRoot_/contentRoot_ are valid before it
    // registers callbacks. It is destroyed first, preventing callbacks from
    // observing a partially destroyed ContentService.
    HotIterationRegistration hotIteration_{this};
};

[[nodiscard]] std::string_view AssetKindName(AssetKind kind) noexcept;
} // namespace orbit::content
