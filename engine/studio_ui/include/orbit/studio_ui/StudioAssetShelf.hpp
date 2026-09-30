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
#include <unordered_map>
#include <unordered_set>
#include <utility>
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
inline u64 thumbnailRevision = ~u64{0};
inline std::unordered_set<std::string> warmedThumbnails;

struct ThumbnailPreview
{
    static constexpr u32 kGrid = 10U;
    std::array<math::Float4, kGrid * kGrid> cells{};
    bool valid{false};
};
inline std::unordered_map<std::string, ThumbnailPreview> thumbnailPreviews;

inline constexpr std::array<std::string_view, 3> kModes{
    "All", "Favorites", "Recent"};
inline constexpr std::array<std::string_view, 12> kKindFilters{
    "Any Type", "Texture", "Material", "Material Instance", "Decal",
    "Component", "Mesh", "Path Profile", "Shader", "Color LUT",
    "Shading Shader", "Shader Material"};

[[nodiscard]] inline bool ContainsPath(
    const std::vector<std::string>& paths,
    const std::string_view path) noexcept
{
    return std::ranges::find(paths, path) != paths.end();
}

inline void SaveState() noexcept
{
    if (statePath.empty()) return;
    try
    {
        std::filesystem::create_directories(statePath.parent_path());
        auto temporary = statePath;
        temporary += ".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) return;
            output << "version=1\n";
            output << "mode=" << mode << '\n';
            output << "kind=" << kindFilter << '\n';
            for (const auto& path : favorites) output << "favorite=" << path << '\n';
            for (const auto& path : recents) output << "recent=" << path << '\n';
        }
        std::error_code error;
        std::filesystem::remove(statePath, error);
        error.clear();
        std::filesystem::rename(temporary, statePath, error);
        if (error) std::filesystem::remove(temporary, error);
    }
    catch (...)
    {
    }
}

inline void LoadState() noexcept
{
    favorites.clear();
    recents.clear();
    mode = 0;
    kindFilter = 0;
    if (statePath.empty()) return;

    try
    {
        std::ifstream input(statePath, std::ios::binary);
        if (!input) return;
        std::string line;
        while (std::getline(input, line))
        {
            const auto split = line.find('=');
            if (split == std::string::npos) continue;
            const std::string key = line.substr(0, split);
            const std::string value = line.substr(split + 1U);
            if (key == "favorite" && !value.empty())
            {
                if (!ContainsPath(favorites, value)) favorites.push_back(value);
            }
            else if (key == "recent" && !value.empty())
            {
                if (!ContainsPath(recents, value)) recents.push_back(value);
            }
            else if (key == "mode")
            {
                try { mode = std::clamp(std::stoi(value), 0, 2); }
                catch (...) { mode = 0; }
            }
            else if (key == "kind")
            {
                try
                {
                    kindFilter = std::clamp(
                        std::stoi(value), 0,
                        static_cast<i32>(kKindFilters.size()) - 1);
                }
                catch (...) { kindFilter = 0; }
            }
        }
        if (recents.size() > 16U) recents.resize(16U);
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
    if (found == favorites.end()) favorites.insert(favorites.begin(), path);
    else favorites.erase(found);
    SaveState();
}

inline void TouchRecent(const std::string& path)
{
    const auto found = std::ranges::find(recents, path);
    if (found != recents.end()) recents.erase(found);
    recents.insert(recents.begin(), path);
    if (recents.size() > 16U) recents.resize(16U);
    SaveState();
}

[[nodiscard]] inline bool MatchesKind(const content::AssetRecord& asset) noexcept
{
    switch (kindFilter)
    {
    case 1: return asset.kind == content::AssetKind::Texture;
    case 2: return asset.kind == content::AssetKind::Material;
    case 3: return asset.kind == content::AssetKind::MaterialInstance;
    case 4: return asset.kind == content::AssetKind::Decal;
    case 5: return asset.kind == content::AssetKind::Component;
    case 6: return asset.kind == content::AssetKind::Mesh;
    case 7: return asset.kind == content::AssetKind::PathProfile;
    case 8: return asset.kind == content::AssetKind::Shader;
    case 9: return asset.kind == content::AssetKind::ColorLut;
    case 10: return asset.kind == content::AssetKind::ShadingShader;
    case 11: return asset.kind == content::AssetKind::ShaderMaterial;
    default: return true;
    }
}

inline void PruneMissingEntries() noexcept
{
    if (contentService == nullptr) return;
    const auto prune = [](std::vector<std::string>& paths)
    {
        std::erase_if(paths, [](const std::string& path)
        {
            return contentService->FindByPath(path) == nullptr;
        });
    };
    const auto oldFavorites = favorites.size();
    const auto oldRecents = recents.size();
    prune(favorites);
    prune(recents);
    if (favorites.size() != oldFavorites || recents.size() != oldRecents) SaveState();
}

[[nodiscard]] inline u16 Read16(
    const std::vector<std::byte>& bytes,
    const std::size_t offset) noexcept
{
    if (offset + 2U > bytes.size()) return 0U;
    return static_cast<u16>(std::to_integer<u8>(bytes[offset])) |
        static_cast<u16>(std::to_integer<u8>(bytes[offset + 1U]) << 8U);
}

[[nodiscard]] inline u32 Read32(
    const std::vector<std::byte>& bytes,
    const std::size_t offset) noexcept
{
    if (offset + 4U > bytes.size()) return 0U;
    u32 value = 0U;
    for (u32 index = 0U; index < 4U; ++index)
        value |= static_cast<u32>(std::to_integer<u8>(bytes[offset + index])) << (index * 8U);
    return value;
}

[[nodiscard]] inline ThumbnailPreview BuildThumbnailPreview(
    const content::AssetRecord& asset) noexcept
{
    ThumbnailPreview preview;
    if (contentService == nullptr) return preview;

    try
    {
        const auto thumbnail = contentService->GetThumbnail(asset.id, 80U, 80U);
        std::ifstream input(thumbnail.path, std::ios::binary);
        if (!input) return preview;
        const std::vector<std::byte> bytes(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
        if (bytes.size() < 54U ||
            std::to_integer<u8>(bytes[0]) != 'B' ||
            std::to_integer<u8>(bytes[1]) != 'M' ||
            Read16(bytes, 28U) != 32U)
            return preview;

        const u32 pixelOffset = Read32(bytes, 10U);
        const u32 width = Read32(bytes, 18U);
        const u32 height = Read32(bytes, 22U);
        if (width == 0U || height == 0U) return preview;
        const u64 required = static_cast<u64>(pixelOffset) +
            static_cast<u64>(width) * height * 4ULL;
        if (required > bytes.size()) return preview;

        for (u32 gy = 0U; gy < ThumbnailPreview::kGrid; ++gy)
        {
            for (u32 gx = 0U; gx < ThumbnailPreview::kGrid; ++gx)
            {
                const u32 sourceX = std::min(width - 1U,
                    (gx * width + width / (ThumbnailPreview::kGrid * 2U)) /
                        ThumbnailPreview::kGrid);
                const u32 topY = std::min(height - 1U,
                    (gy * height + height / (ThumbnailPreview::kGrid * 2U)) /
                        ThumbnailPreview::kGrid);
                const u32 fileY = height - 1U - topY;
                const std::size_t offset = static_cast<std::size_t>(pixelOffset) +
                    (static_cast<std::size_t>(fileY) * width + sourceX) * 4U;
                const f32 b = std::to_integer<u8>(bytes[offset + 0U]) / 255.0F;
                const f32 g = std::to_integer<u8>(bytes[offset + 1U]) / 255.0F;
                const f32 r = std::to_integer<u8>(bytes[offset + 2U]) / 255.0F;
                preview.cells[gy * ThumbnailPreview::kGrid + gx] = {r, g, b, 1.0F};
            }
        }
        preview.valid = true;
    }
    catch (...)
    {
    }
    return preview;
}

inline void SyncThumbnailRevision() noexcept
{
    if (contentService == nullptr) return;
    const u64 revision = contentService->Revision();
    if (thumbnailRevision == revision) return;
    thumbnailRevision = revision;
    warmedThumbnails.clear();
    thumbnailPreviews.clear();
    PruneMissingEntries();
}

[[nodiscard]] inline const ThumbnailPreview& PreviewFor(
    const content::AssetRecord& asset)
{
    SyncThumbnailRevision();
    const std::string path = asset.sourcePath.generic_string();
    const auto found = thumbnailPreviews.find(path);
    if (found != thumbnailPreviews.end()) return found->second;
    return thumbnailPreviews.emplace(path, BuildThumbnailPreview(asset)).first->second;
}

[[nodiscard]] inline editor_ui::CanvasInteraction DrawThumbnail(
    editor_ui::PanelContext& context,
    const content::AssetRecord& asset)
{
    const f32 scale = editor_ui::CurrentUiScale();
    const f32 pixels = 72.0F * scale;
    std::string id = "asset-thumb-" + asset.id.ToString();
    const auto interaction = context.Canvas(id, {.width = pixels, .height = pixels});
    const auto& preview = PreviewFor(asset);
    if (!preview.valid)
    {
        context.CanvasText({0.10F, 0.42F}, {0.65F, 0.70F, 0.78F, 1.0F}, content::AssetKindName(asset.kind));
        return interaction;
    }

    constexpr f32 grid = static_cast<f32>(ThumbnailPreview::kGrid);
    const f32 thickness = pixels / grid + 1.0F;
    for (u32 y = 0U; y < ThumbnailPreview::kGrid; ++y)
    {
        for (u32 x = 0U; x < ThumbnailPreview::kGrid; ++x)
        {
            const f32 x0 = static_cast<f32>(x) / grid;
            const f32 x1 = static_cast<f32>(x + 1U) / grid;
            const f32 cy = (static_cast<f32>(y) + 0.5F) / grid;
            context.CanvasLine(
                {x0, cy}, {x1, cy},
                preview.cells[y * ThumbnailPreview::kGrid + x],
                thickness);
        }
    }
    return interaction;
}

inline void AppendPinnedAssets(
    std::vector<content::AssetRecord>& visible,
    const std::vector<std::string>& paths,
    const std::vector<content::AssetRecord>& searchResults)
{
    for (const std::string& path : paths)
    {
        const auto* asset = contentService->FindByPath(path);
        if (asset == nullptr || !MatchesKind(*asset)) continue;
        if (!query.empty() &&
            std::ranges::find_if(searchResults, [asset](const content::AssetRecord& item)
            {
                return item.id == asset->id;
            }) == searchResults.end())
            continue;
        visible.push_back(*asset);
        if (visible.size() >= 64U) break;
    }
}

inline void Draw(editor_ui::PanelContext& context)
{
    if (contentService == nullptr)
    {
        context.MutedText("Project content is unavailable.");
        return;
    }

    SyncThumbnailRevision();

    const i32 previousMode = mode;
    static_cast<void>(context.SegmentedControl("asset-shelf-mode", kModes, mode));
    if (mode != previousMode) SaveState();

    const i32 previousKind = kindFilter;
    static_cast<void>(context.Combo("Type##asset-shelf-type", kKindFilters, kindFilter));
    if (kindFilter != previousKind) SaveState();

    static_cast<void>(context.InputText("Search##asset-shelf-search", query));
    if (!query.empty() || kindFilter != 0)
    {
        context.SameLine();
        if (context.Button("Reset##asset-shelf-reset-filters"))
        {
            query.clear();
            kindFilter = 0;
            SaveState();
        }
    }
    if (mode == 2 && !recents.empty())
    {
        context.SameLine();
        if (context.Button("Clear Recent##asset-shelf-clear-recent"))
        {
            recents.clear();
            SaveState();
        }
    }

    const auto assets = contentService->Search(query);
    std::vector<content::AssetRecord> visible;
    visible.reserve(std::min<std::size_t>(assets.size(), 64U));
    if (mode == 1) AppendPinnedAssets(visible, favorites, assets);
    else if (mode == 2) AppendPinnedAssets(visible, recents, assets);
    else
    {
        for (const auto& asset : assets)
        {
            if (!MatchesKind(asset)) continue;
            visible.push_back(asset);
            if (visible.size() >= 64U) break;
        }
    }

    context.Separator();
    context.MutedText(std::to_string(visible.size()) + (visible.size() == 1U ? " asset" : " assets"));
    if (visible.empty())
    {
        context.MutedText(mode == 1
            ? "No favorite assets match the current filters."
            : mode == 2
                ? "No recent assets match the current filters."
                : "No project assets match the current filters.");
        return;
    }

    for (const auto& asset : visible)
    {
        const std::string path = asset.sourcePath.generic_string();
        const bool favorite = ContainsPath(favorites, path);
        const auto thumbnail = DrawThumbnail(context, asset);
        if (thumbnail.clicked || thumbnail.doubleClicked) TouchRecent(path);

        if (context.BeginDragSource())
        {
            const std::span<const char> characters(path.data(), path.size());
            context.SetDragPayload("ORBIT_ASSET_PATH", std::as_bytes(characters));
            context.Text(asset.name);
            context.MutedText(path);
            context.EndDragSource();
            TouchRecent(path);
        }

        std::vector<editor_ui::ActionPresentation> actions;
        actions.push_back({
            .label = favorite ? "Remove Favorite" : "Add Favorite",
            .invoke = [path] { ToggleFavorite(path); }
        });
        actions.push_back({
            .label = "Add to Recent",
            .invoke = [path] { TouchRecent(path); }
        });
        context.ContextMenu(
            "asset-context-" + asset.id.ToString(),
            actions,
            thumbnail.rightClicked);

        context.SameLine();
        std::string label = asset.name + "\n" + std::string(content::AssetKindName(asset.kind));
        if (asset.kind == content::AssetKind::ShadingShader) label += " (Shading)";
        label += " · " + path + "##asset-shelf-item-" + asset.id.ToString();
        if (context.Selectable(label, false)) TouchRecent(path);

        context.SameLine();
        std::string favoriteLabel = favorite ? "Fav" : "+Fav";
        favoriteLabel += "##asset-shelf-favorite-" + asset.id.ToString();
        if (context.Button(favoriteLabel)) ToggleFavorite(path);
    }
}
} // namespace asset_shelf_detail

inline void MarkStudioAssetRecent(const std::filesystem::path& path) noexcept
{
    if (!path.empty()) asset_shelf_detail::TouchRecent(path.generic_string());
}

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
        asset_shelf_detail::thumbnailRevision = ~u64{0};
        asset_shelf_detail::warmedThumbnails.clear();
        asset_shelf_detail::thumbnailPreviews.clear();
        asset_shelf_detail::LoadState();
        asset_shelf_detail::PruneMissingEntries();
    }

    try
    {
        GlobalInspectorProviders().Upsert({
            .id = "orbit.assets.shelf",
            .owner = "orbit",
            .title = "Assets",
            .order = 100,
            .defaultOpen = false,
            .relevant = [] { return asset_shelf_detail::contentService != nullptr; },
            .draw = [](editor_ui::PanelContext& context) { asset_shelf_detail::Draw(context); }
        });
    }
    catch (...)
    {
    }
}
} // namespace orbit::studio_ui
