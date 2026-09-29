#include <orbit/studio_ui/StudioAssetShelf.hpp>

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
namespace
{
content::ContentService* g_content = nullptr;
std::string g_query;
i32 g_mode = 0;
std::vector<std::string> g_favorites;
std::vector<std::string> g_recents;

constexpr std::array<std::string_view, 3> kModes{
    "All",
    "Favorites",
    "Recent"
};

[[nodiscard]] bool ContainsPath(
    const std::vector<std::string>& paths,
    const std::string_view path) noexcept
{
    return std::ranges::find(paths, path) != paths.end();
}

void ToggleFavorite(const std::string& path)
{
    const auto found = std::ranges::find(g_favorites, path);
    if (found == g_favorites.end())
    {
        g_favorites.insert(g_favorites.begin(), path);
    }
    else
    {
        g_favorites.erase(found);
    }
}

void TouchRecent(const std::string& path)
{
    const auto found = std::ranges::find(g_recents, path);
    if (found != g_recents.end())
    {
        g_recents.erase(found);
    }
    g_recents.insert(g_recents.begin(), path);
    constexpr std::size_t kMaxRecent = 16U;
    if (g_recents.size() > kMaxRecent)
    {
        g_recents.resize(kMaxRecent);
    }
}

[[nodiscard]] bool VisibleInMode(const std::string_view path) noexcept
{
    switch (g_mode)
    {
    case 1:
        return ContainsPath(g_favorites, path);
    case 2:
        return ContainsPath(g_recents, path);
    default:
        return true;
    }
}

void DrawAssetShelf(editor_ui::PanelContext& context)
{
    if (g_content == nullptr)
    {
        context.MutedText("Project content is unavailable.");
        return;
    }

    static_cast<void>(
        context.SegmentedControl(
            "asset-shelf-mode",
            kModes,
            g_mode));
    static_cast<void>(
        context.InputText(
            "Search##asset-shelf-search",
            g_query));

    auto assets = g_content->Search(g_query);
    std::vector<content::AssetRecord> visible;
    visible.reserve(std::min<std::size_t>(assets.size(), 64U));

    if (g_mode == 2)
    {
        // Recents are intentionally ordered by use, not alphabetically/search
        // index order. This keeps the shelf useful for repetitive authoring.
        for (const std::string& recentPath : g_recents)
        {
            const auto* asset = g_content->FindByPath(recentPath);
            if (asset == nullptr)
            {
                continue;
            }
            if (!g_query.empty())
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
            g_mode == 1
                ? "No favorite assets match the current search."
                : g_mode == 2
                    ? "No recent assets match the current search."
                    : "No project assets match the current search.");
        return;
    }

    context.MutedText(
        "Click to mark recent. Drag an asset onto compatible authoring surfaces.");

    for (const auto& asset : visible)
    {
        const std::string path = asset.sourcePath.generic_string();
        const bool favorite = ContainsPath(g_favorites, path);

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

        // Warm the shared project thumbnail cache while this card is visible.
        // The current generic PanelContext has no file-backed image primitive,
        // so the shelf keeps thumbnail generation authoritative here and can
        // switch to direct image cards without another content API change.
        const auto thumbnail = g_content->GetThumbnail(asset.id, 72U, 72U);
        context.MutedText(
            std::string("Thumbnail ") +
            std::to_string(thumbnail.width) + "×" +
            std::to_string(thumbnail.height) +
            (thumbnail.cacheHit ? " · cached" : " · generated"));
    }
}
} // namespace

void InstallStudioAssetShelf(content::ContentService* content) noexcept
{
    g_content = content;

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
                return g_content != nullptr;
            },
            .draw = [](editor_ui::PanelContext& context)
            {
                DrawAssetShelf(context);
            }
        });
    }
    catch (...)
    {
        // Content binding must not make Studio construction fail. If the
        // presentation registry is unavailable (for example in a headless
        // model test), the ContentService remains bound to viewport tools.
    }
}
} // namespace orbit::studio_ui
