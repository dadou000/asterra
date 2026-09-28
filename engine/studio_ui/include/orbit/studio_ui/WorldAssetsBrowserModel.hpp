#pragma once

#include <orbit/content/ContentService.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace orbit::studio_ui
{
enum class AssetBrowserCategory : u8
{
    All,
    Materials,
    Models,
    Blueprints,
    Vehicles,
    Buildings,
    Procedural,
    Textures,
    Decals,
    Shaders,
    Favorites,
    Recent
};

[[nodiscard]] inline std::string BrowserLower(
    const std::string_view text)
{
    std::string result;
    result.reserve(text.size());
    for (const unsigned char value : text)
    {
        result.push_back(
            static_cast<char>(std::tolower(value)));
    }
    return result;
}

[[nodiscard]] inline bool AssetHasTag(
    const content::AssetRecord& asset,
    const std::string_view expected)
{
    const std::string lowerExpected = BrowserLower(expected);
    return std::ranges::any_of(
        asset.tags,
        [&lowerExpected](const std::string& tag)
        {
            return BrowserLower(tag) == lowerExpected;
        });
}

class WorldAssetsBrowserModel
{
public:
    void SetQuery(std::string query)
    {
        query_ = std::move(query);
    }

    [[nodiscard]] const std::string& Query() const noexcept
    {
        return query_;
    }

    void SetCategory(const AssetBrowserCategory category) noexcept
    {
        category_ = category;
    }

    [[nodiscard]] AssetBrowserCategory Category() const noexcept
    {
        return category_;
    }

    void SetFolder(std::optional<std::filesystem::path> folder)
    {
        folder_ = std::move(folder);
    }

    [[nodiscard]] const std::optional<std::filesystem::path>& Folder() const noexcept
    {
        return folder_;
    }

    void SetSearchChips(std::vector<std::string> chips)
    {
        searchChips_ = std::move(chips);
    }

    [[nodiscard]] const std::vector<std::string>& SearchChips() const noexcept
    {
        return searchChips_;
    }

    void ToggleFavorite(const content::AssetId asset)
    {
        if (!favorites_.erase(asset))
        {
            favorites_.insert(asset);
        }
    }

    [[nodiscard]] bool IsFavorite(const content::AssetId asset) const noexcept
    {
        return favorites_.contains(asset);
    }

    void RecordRecent(const content::AssetId asset)
    {
        std::erase(recents_, asset);
        recents_.insert(recents_.begin(), asset);
        constexpr std::size_t kRecentLimit = 24U;
        if (recents_.size() > kRecentLimit)
        {
            recents_.resize(kRecentLimit);
        }
    }

    [[nodiscard]] const std::vector<content::AssetId>& Recent() const noexcept
    {
        return recents_;
    }

    [[nodiscard]] bool Matches(
        const content::AssetRecord& asset) const
    {
        if (!MatchesCategory(asset))
        {
            return false;
        }

        if (folder_.has_value())
        {
            const auto parent =
                asset.sourcePath.parent_path().lexically_normal();
            if (parent != folder_->lexically_normal())
            {
                return false;
            }
        }

        const std::string searchable = BrowserLower(
            asset.name + " " + asset.sourcePath.generic_string());
        const std::string lowerQuery = BrowserLower(query_);
        if (!lowerQuery.empty() &&
            searchable.find(lowerQuery) == std::string::npos &&
            !std::ranges::any_of(
                asset.tags,
                [&lowerQuery](const std::string& tag)
                {
                    return BrowserLower(tag).find(lowerQuery) !=
                        std::string::npos;
                }))
        {
            return false;
        }

        for (const std::string& chip : searchChips_)
        {
            const std::string lowerChip = BrowserLower(chip);
            if (lowerChip.empty())
            {
                continue;
            }

            const bool found =
                searchable.find(lowerChip) != std::string::npos ||
                std::ranges::any_of(
                    asset.tags,
                    [&lowerChip](const std::string& tag)
                    {
                        return BrowserLower(tag).find(lowerChip) !=
                            std::string::npos;
                    });
            if (!found)
            {
                return false;
            }
        }

        return true;
    }

    [[nodiscard]] std::vector<content::AssetRecord> Filter(
        const std::vector<content::AssetRecord>& assets) const
    {
        std::vector<content::AssetRecord> result;
        for (const auto& asset : assets)
        {
            if (Matches(asset))
            {
                result.push_back(asset);
            }
        }
        return result;
    }

private:
    [[nodiscard]] bool MatchesCategory(
        const content::AssetRecord& asset) const
    {
        using content::AssetKind;

        switch (category_)
        {
        case AssetBrowserCategory::All:
            return true;
        case AssetBrowserCategory::Materials:
            return asset.kind == AssetKind::Material ||
                asset.kind == AssetKind::MaterialInstance ||
                asset.kind == AssetKind::ShaderMaterial;
        case AssetBrowserCategory::Models:
            return asset.kind == AssetKind::Mesh ||
                asset.kind == AssetKind::Component;
        case AssetBrowserCategory::Blueprints:
            return AssetHasTag(asset, "blueprint");
        case AssetBrowserCategory::Vehicles:
            return AssetHasTag(asset, "vehicle");
        case AssetBrowserCategory::Buildings:
            return AssetHasTag(asset, "building");
        case AssetBrowserCategory::Procedural:
            return AssetHasTag(asset, "procedural") ||
                asset.kind == AssetKind::PathProfile;
        case AssetBrowserCategory::Textures:
            return asset.kind == AssetKind::Texture;
        case AssetBrowserCategory::Decals:
            return asset.kind == AssetKind::Decal;
        case AssetBrowserCategory::Shaders:
            return asset.kind == AssetKind::Shader ||
                asset.kind == AssetKind::ShadingShader;
        case AssetBrowserCategory::Favorites:
            return favorites_.contains(asset.id);
        case AssetBrowserCategory::Recent:
            return std::ranges::find(recents_, asset.id) != recents_.end();
        }
        return true;
    }

    std::string query_;
    AssetBrowserCategory category_{AssetBrowserCategory::All};
    std::optional<std::filesystem::path> folder_;
    std::vector<std::string> searchChips_;
    std::unordered_set<content::AssetId> favorites_;
    std::vector<content::AssetId> recents_;
};
} // namespace orbit::studio_ui
