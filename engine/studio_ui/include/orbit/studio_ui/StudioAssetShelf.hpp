#pragma once

#include <orbit/content/ContentService.hpp>
#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/studio_ui/StudioExpansionShell.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::studio_ui
{
namespace asset_shelf_detail
{
inline content::ContentService* contentService = nullptr;
inline std::string query;
inline i32 mode = 0;
inline std::vector<std::string> favorites;
inline std::vector<std::string> recents;

inline constexpr std::array<std::string_view, 3> kModes{
    "All",
    "Favorites",
    "Recent"
};

[[nodiscard]] inline bool ContainsPath(
    const std::vector<std::string>& paths,
    const std::string_view path) noexcept
{
    return std::ranges::find(paths, path) != paths.end();
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

inline void Draw(editor_ui::PanelContext& context)
{
    if (contentService == nullptr)
    {
        context.MutedText("Project content is unavailable.");
        return;
    }

    static_cast<void>(
        context.SegmentedControl(
            "asset-shelf-mode",
            kModes,
            mode));
    static_cast<void>(
        context.InputText(
            "Search##asset-shelf-search",
            query));

    auto assets = contentService->Search(query);
    std::vector<content::AssetRecord> visible;
    visible.reserve(std::min<std::size_t>(assets.size(), 64U));

    if (mode == 2)
    {
        for (const std::string& recentPath : recents)
        {
            const auto* asset = contentService->FindByPath(recentPath);
            if (asset == nullptr)
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
            if (!VisibleInMode(path))
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
    if (visible.empty())
    {
        context.MutedText(
            mode == 1
                ? "No favorite assets match the current search."
                : mode == 2
                    ? "No recent assets match the current search."
                    : "No project assets match the current search.");
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

// Binds the project content catalog to Orbit's canonical Properties surface.
// It stays inside the unified Studio shell rather than introducing another
// standalone launcher or specialist panel.
inline void InstallStudioAssetShelf(
    content::ContentService* content) noexcept
{
    asset_shelf_detail::contentService = content;

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
