#pragma once

#include <orbit/content/ContentService.hpp>
#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/studio_ui/StudioExpansionShell.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::studio_ui
{
namespace asset_shelf_detail
{
inline content::ContentService* contentService = nullptr;
inline std::filesystem::path statePath;
inline std::string query;
inline i32 mode = 0;
inline i32 kindFilter = 0;
inline std::vector<std::string> favorites;
inline std::vector<std::string> recents;

inline constexpr std::array<std::string_view, 3> kModes{
    "All",
    "Favorites",
    "Recent"
};

inline constexpr std::array<std::string_view, 12> kKindFilters{
    "Any Type",
    "Texture",
    "Material",
    "Material Instance",
    "Decal",
    "Component",
    "Mesh",
    "Path Profile",
    "Shader",
    "Color LUT",
    "Shading Shader",
    "Shader Material"
};

[[nodiscard]] inline bool ContainsPath(
    const std::vector<std::string>& paths,
    const std::string_view path) noexcept
{
    return std::ranges::find(paths, path) != paths.end();
}

inline void SaveState() noexcept
{
    if (statePath.empty())
    {
        return;
    }

    try
    {
        std::filesystem::create_directories(statePath.parent_path());
        auto temporary = statePath;
        temporary += ".tmp";

        {
            std::ofstream output(
                temporary,
                std::ios::binary | std::ios::trunc);
            if (!output)
            {
                return;
            }

            output << "version=1\n";
            output << "mode=" << mode << '\n';
            output << "kind=" << kindFilter << '\n';
            for (const std::string& path : favorites)
            {
                output << "favorite=" << path << '\n';
            }
            for (const std::string& path : recents)
            {
                output << "recent=" << path << '\n';
            }
        }

        std::error_code error;
        std::filesystem::remove(statePath, error);
        error.clear();
        std::filesystem::rename(temporary, statePath, error);
        if (error)
        {
            std::filesystem::remove(temporary, error);
        }
    }
    catch (...)
    {
        // Asset browsing preference persistence is QoL only and must never
        // interfere with authoring or project opening.
    }
}

inline void LoadState() noexcept
{
    favorites.clear();
    recents.clear();
    mode = 0;
    kindFilter = 0;

    if (statePath.empty())
    {
        return;
    }

    try
    {
        std::ifstream input(statePath, std::ios::binary);
        if (!input)
        {
            return;
        }

        std::string line;
        while (std::getline(input, line))
        {
            const auto split = line.find('=');
            if (split == std::string::npos)
            {
                continue;
            }

            const std::string key = line.substr(0, split);
            const std::string value = line.substr(split + 1U);
            if (key == "favorite" && !value.empty())
            {
                if (!ContainsPath(favorites, value))
                {
                    favorites.push_back(value);
                }
            }
            else if (key == "recent" && !value.empty())
            {
                if (!ContainsPath(recents, value))
                {
                    recents.push_back(value);
                }
            }
            else if (key == "mode")
            {
                try
                {
                    mode = std::clamp(std::stoi(value), 0, 2);
                }
                catch (...)
                {
                    mode = 0;
                }
            }
            else if (key == "kind")
            {
                try
                {
                    kindFilter = std::clamp(
                        std::stoi(value),
                        0,
                        static_cast<i32>(kKindFilters.size()) - 1);
                }
                catch (...)
                {
                    kindFilter = 0;
                }
            }
        }

        constexpr std::size_t kMaxRecent = 16U;
        if (recents.size() > kMaxRecent)
        {
            recents.resize(kMaxRecent);
        }
    }
    catch (...)
    {
        favorites.clear();
        recents.clear();
        mode = 0;
        kindFilter = 0;
    }
}

inline void ToggleFavorite(const std::string& path)
{
    const auto found = std::ranges::find(favorites, path);
    if (found == favorites.end())
    {
        favorites.insert(favorites.begin(), path);
    }
    else
    {
        favorites.erase(found);
    }
    SaveState();
}

inline void TouchRecent(const std::string& path)
{
    const auto found = std::ranges::find(recents, path);
    if (found != recents.end())
    {
        recents.erase(found);
    }
    recents.insert(recents.begin(), path);
    constexpr std::size_t kMaxRecent = 16U;
    if (recents.size() > kMaxRecent)
    {
        recents.resize(kMaxRecent);
    }
    SaveState();
}

[[nodiscard]] inline bool VisibleInMode(
    const std::string_view path) noexcept
{
    switch (mode)
    {
    case 1: return ContainsPath(favorites, path);
    case 2: return ContainsPath(recents, path);
    default: return true;
    }
}

[[nodiscard]] inline bool MatchesKind(
    const content::AssetRecord& asset) noexcept
{
    if (kindFilter <= 0)
    {
        return true;
    }

    return content::AssetKindName(asset.kind) ==
        kKindFilters[static_cast<std::size_t>(kindFilter)];
}

inline void Draw(editor_ui::PanelContext& context)
{
    if (contentService == nullptr)
    {
        context.MutedText("Project content is unavailable.");
        return;
    }

    const i32 previousMode = mode;
    static_cast<void>(
        context.SegmentedControl(
            "asset-shelf-mode",
            kModes,
            mode));
    if (mode != previousMode)
    {
        SaveState();
    }

    const i32 previousKind = kindFilter;
    static_cast<void>(
        context.Combo(
            "Type##asset-shelf-type",
            kKindFilters,
            kindFilter));
    if (kindFilter != previousKind)
    {
        SaveState();
    }

    static_cast<void>(
        context.InputText(
            "Search##asset-shelf-search",
            query));
    if (!query.empty())
    {
        context.SameLine();
        if (context.Button("Clear##asset-shelf-clear-search"))
        {
            query.clear();
        }
    }

    auto assets = contentService->Search(query);
    std::vector<content::AssetRecord> visible;
    visible.reserve(std::min<std::size_t>(assets.size(), 64U));

    if (mode == 2)
    {
        for (const std::string& recentPath : recents)
        {
            const auto* asset = contentService->FindByPath(recentPath);
            if (asset == nullptr || !MatchesKind(*asset))
            {
                continue;
            }
            if (!query.empty())
            {
                const auto match = std::ranges::find_if(
                    assets,
                    [asset](const content::AssetRecord& candidate)
                    {
                        return candidate.id == asset->id;
                    });
                if (match == assets.end())
                {
                    continue;
                }
            }
            visible.push_back(*asset);
            if (visible.size() >= 64U)
            {
                break;
            }
        }
    }
    else
    {
        for (auto& asset : assets)
        {
            const std::string path = asset.sourcePath.generic_string();
            if (!VisibleInMode(path) || !MatchesKind(asset))
            {
                continue;
            }
            visible.push_back(std::move(asset));
            if (visible.size() >= 64U)
            {
                break;
            }
        }
    }

    context.Separator();
    context.MutedText(
        std::to_string(visible.size()) +
        (visible.size() == 1U ? " asset" : " assets"));

    if (visible.empty())
    {
        context.MutedText(
            mode == 1
                ? "No favorite assets match the current filters."
                : mode == 2
                    ? "No recent assets match the current filters."
                    : "No project assets match the current filters.");
        return;
    }

    context.MutedText(
        "Click to mark recent. Drag an asset onto compatible authoring surfaces.");

    for (const auto& asset : visible)
    {
        const std::string path = asset.sourcePath.generic_string();
        const bool favorite = ContainsPath(favorites, path);

        std::string label = asset.name;
        label += "\n";
        label += content::AssetKindName(asset.kind);
        label += " · ";
        label += path;
        label += "##asset-shelf-item-";
        label += asset.id.ToString();

        if (context.Selectable(label, false))
        {
            TouchRecent(path);
        }

        if (context.BeginDragSource())
        {
            const std::span<const char> characters(
                path.data(),
                path.size());
            context.SetDragPayload(
                "ORBIT_ASSET_PATH",
                std::as_bytes(characters));
            context.Text(asset.name);
            context.MutedText(path);
            context.EndDragSource();
            TouchRecent(path);
        }

        context.SameLine();
        std::string favoriteLabel = favorite ? "★" : "☆";
        favoriteLabel += "##asset-shelf-favorite-";
        favoriteLabel += asset.id.ToString();
        if (context.Button(favoriteLabel))
        {
            ToggleFavorite(path);
        }

        const auto thumbnail =
            contentService->GetThumbnail(asset.id, 72U, 72U);
        context.MutedText(
            std::string("Thumbnail ") +
            std::to_string(thumbnail.width) + "×" +
            std::to_string(thumbnail.height) +
            (thumbnail.cacheHit ? " · cached" : " · generated"));
    }
}
} // namespace asset_shelf_detail

inline void MarkStudioAssetRecent(const std::filesystem::path& path) noexcept
{
    if (!path.empty())
    {
        asset_shelf_detail::TouchRecent(path.generic_string());
    }
}

// Binds the project content catalog to Orbit's canonical Properties surface.
// The shelf persists lightweight browsing preferences per project in .orbit.
inline void InstallStudioAssetShelf(
    content::ContentService* content,
    std::filesystem::path persistencePath = {}) noexcept
{
    const bool bindingChanged =
        asset_shelf_detail::contentService != content ||
        asset_shelf_detail::statePath != persistencePath;

    asset_shelf_detail::contentService = content;
    asset_shelf_detail::statePath = std::move(persistencePath);
    if (bindingChanged)
    {
        asset_shelf_detail::query.clear();
        asset_shelf_detail::LoadState();
    }

    try
    {
        GlobalInspectorProviders().Upsert({
            .id = "orbit.assets.shelf",
            .owner = "orbit",
            .title = "Assets",
            .order = 100,
            .defaultOpen = false,
            .relevant = []
            {
                return asset_shelf_detail::contentService != nullptr;
            },
            .draw = [](editor_ui::PanelContext& context)
            {
                asset_shelf_detail::Draw(context);
            }
        });
    }
    catch (...)
    {
        // Headless/model tests may construct the viewport shell without an
        // active presentation registry. Content binding remains valid there.
    }
}
} // namespace orbit::studio_ui
