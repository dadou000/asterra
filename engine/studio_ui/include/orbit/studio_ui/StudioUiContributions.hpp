#pragma once

#include <orbit/commands/CommandRegistry.hpp>
#include <orbit/editor_ui/EditorUi.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orbit::studio_ui
{
enum class StudioContributionSurface : u8
{
    WorkspaceToolbar,
    ContextToolbar,
    Inspector,
    Browser,
    Activity,
    QuickCreate
};

enum class StudioContributionKind : u8
{
    Command,
    EmbeddedPanel,
    BrowserCategory
};

inline constexpr std::string_view kStudioWorkspaceCommandSurface{
    "studio.workspace"};
inline constexpr std::string_view kStudioContextCommandSurface{
    "studio.context"};
inline constexpr std::string_view kStudioQuickCreateCommandSurface{
    "studio.quick_create"};
inline constexpr std::string_view kStudioBrowserCommandSurface{
    "studio.browser"};
inline constexpr std::string_view kStudioActivityCommandSurface{
    "studio.activity"};

struct StudioUiContribution
{
    std::string id;
    std::string owner{"orbit"};
    std::string label;
    StudioContributionSurface surface{
        StudioContributionSurface::ContextToolbar};
    StudioContributionKind kind{
        StudioContributionKind::Command};
    i32 order{100};
    commands::CommandId command{};
    editor_ui::PanelId panel{};
    std::string category;
};

class StudioUiContributionRegistry
{
public:
    void Upsert(StudioUiContribution contribution)
    {
        if (contribution.id.empty())
        {
            throw std::invalid_argument(
                "Studio UI contribution id cannot be empty.");
        }

        const auto existing = std::ranges::find_if(
            entries_,
            [&contribution](const Entry& entry)
            {
                return entry.definition.id == contribution.id;
            });

        if (existing != entries_.end())
        {
            existing->definition = std::move(contribution);
            ++revision_;
            return;
        }

        entries_.push_back({
            .definition = std::move(contribution),
            .sequence = nextSequence_++
        });
        ++revision_;
    }

    [[nodiscard]] bool Remove(
        const std::string_view id) noexcept
    {
        const auto existing = std::ranges::find_if(
            entries_,
            [id](const Entry& entry)
            {
                return entry.definition.id == id;
            });

        if (existing == entries_.end())
        {
            return false;
        }

        entries_.erase(existing);
        ++revision_;
        return true;
    }

    [[nodiscard]] u32 RemoveOwner(
        const std::string_view owner) noexcept
    {
        const auto before = entries_.size();
        std::erase_if(
            entries_,
            [owner](const Entry& entry)
            {
                return entry.definition.owner == owner;
            });

        const auto removed = before - entries_.size();
        if (removed != 0U)
        {
            ++revision_;
        }
        return static_cast<u32>(removed);
    }

    [[nodiscard]] std::vector<StudioUiContribution> Catalog(
        const StudioContributionSurface surface) const
    {
        std::vector<const Entry*> filtered;
        for (const Entry& entry : entries_)
        {
            if (entry.definition.surface == surface)
            {
                filtered.push_back(&entry);
            }
        }

        std::ranges::stable_sort(
            filtered,
            [](const Entry* left, const Entry* right)
            {
                if (left->definition.order != right->definition.order)
                {
                    return left->definition.order < right->definition.order;
                }
                return left->sequence < right->sequence;
            });

        std::vector<StudioUiContribution> result;
        result.reserve(filtered.size());
        for (const Entry* entry : filtered)
        {
            result.push_back(entry->definition);
        }
        return result;
    }

    [[nodiscard]] u64 Revision() const noexcept
    {
        return revision_;
    }

private:
    struct Entry
    {
        StudioUiContribution definition;
        u64 sequence{0};
    };

    std::vector<Entry> entries_;
    u64 nextSequence_{0};
    u64 revision_{0};
};
} // namespace orbit::studio_ui
