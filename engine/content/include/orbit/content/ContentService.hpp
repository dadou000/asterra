#pragma once

#include <orbit/core/StrongId.hpp>
#include <orbit/core/Types.hpp>

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
    Decal,
    Component,
    Mesh,
    Unknown
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
};

struct AssetRecord
{
    AssetId id{};
    AssetKind kind{AssetKind::Unknown};
    std::string name;
    std::filesystem::path sourcePath;
    std::vector<std::string> tags;
    std::optional<MaterialChannels> material;
};

class ContentService
{
public:
    explicit ContentService(std::filesystem::path projectRoot);

    // Rebuilds the in-memory index from project Content mounts. IDs are
    // derived from normalized project-relative paths and remain stable.
    void Scan();

    [[nodiscard]] const AssetRecord* Find(AssetId id) const noexcept;
    [[nodiscard]] const AssetRecord* FindByPath(const std::filesystem::path& path) const noexcept;
    [[nodiscard]] std::vector<AssetRecord> Search(std::string_view query, std::optional<AssetKind> kind = std::nullopt) const;
    [[nodiscard]] std::vector<AssetRecord> All() const;
    [[nodiscard]] u64 Revision() const noexcept;

    // Imports a source file into Content/Imported without temporary staging
    // formats. The copied source becomes the canonical project asset.
    [[nodiscard]] AssetId ImportFile(const std::filesystem::path& source);

private:
    [[nodiscard]] AssetRecord BuildRecord(const std::filesystem::path& absolute) const;
    [[nodiscard]] AssetId StableId(const std::filesystem::path& absolute) const;
    [[nodiscard]] std::filesystem::path NormalizeInsideProject(const std::filesystem::path& path) const;

    std::filesystem::path projectRoot_;
    std::filesystem::path contentRoot_;
    std::unordered_map<AssetId, AssetRecord> assets_;
    std::unordered_map<std::string, AssetId> pathIndex_;
    u64 revision_{0};
};

[[nodiscard]] std::string_view AssetKindName(AssetKind kind) noexcept;
} // namespace orbit::content
