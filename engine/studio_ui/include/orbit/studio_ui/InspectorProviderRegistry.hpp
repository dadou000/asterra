#pragma once

#include <orbit/editor_ui/EditorUi.hpp>

#include <algorithm>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orbit::studio_ui
{
struct InspectorProviderDefinition
{
    std::string id;
    std::string owner{"orbit"};
    std::string title;
    i32 order{100};
    bool defaultOpen{false};
    std::function<bool()> relevant;
    std::function<void(editor_ui::PanelContext&)> draw;
};

class InspectorProviderRegistry
{
public:
    void Upsert(InspectorProviderDefinition provider)
    {
        if (provider.id.empty())
        {
            throw std::invalid_argument(
                "Inspector provider id cannot be empty.");
        }

        if (!provider.draw)
        {
            throw std::invalid_argument(
                "Inspector provider requires a draw callback.");
        }

        const auto existing = std::ranges::find_if(
            providers_,
            [&provider](const Entry& entry)
            {
                return entry.definition.id == provider.id;
            });

        if (existing != providers_.end())
        {
            existing->definition = std::move(provider);
            ++revision_;
            return;
        }

        providers_.push_back({
            .definition = std::move(provider),
            .sequence = nextSequence_++
        });
        ++revision_;
    }

    [[nodiscard]] bool Remove(
        const std::string_view id) noexcept
    {
        const auto existing = std::ranges::find_if(
            providers_,
            [id](const Entry& entry)
            {
                return entry.definition.id == id;
            });

        if (existing == providers_.end())
        {
            return false;
        }

        providers_.erase(existing);
        ++revision_;
        return true;
    }

    [[nodiscard]] u32 RemoveOwner(
        const std::string_view owner) noexcept
    {
        const auto before = providers_.size();
        std::erase_if(
            providers_,
            [owner](const Entry& entry)
            {
                return entry.definition.owner == owner;
            });

        const auto removed = before - providers_.size();
        if (removed != 0U)
        {
            ++revision_;
        }

        return static_cast<u32>(removed);
    }

    void Clear() noexcept
    {
        if (!providers_.empty())
        {
            providers_.clear();
            ++revision_;
        }
    }

    [[nodiscard]] u64 Revision() const noexcept
    {
        return revision_;
    }

    // Lightweight read-only view for diagnostics/tests. Relevance callbacks
    // are evaluated from value snapshots, so a provider may trigger its own
    // reload/removal without invalidating this traversal. Returned pointers
    // refer to the providers that still exist after those callbacks finish and
    // must not be retained across later registry mutation.
    [[nodiscard]] std::vector<const InspectorProviderDefinition*>
    Relevant() const
    {
        std::vector<Entry> snapshot = providers_;
        std::ranges::stable_sort(
            snapshot,
            [](const Entry& left, const Entry& right)
            {
                if (left.definition.order != right.definition.order)
                {
                    return left.definition.order < right.definition.order;
                }
                return left.sequence < right.sequence;
            });

        std::vector<std::string> relevantIds;
        relevantIds.reserve(snapshot.size());

        for (const Entry& entry : snapshot)
        {
            bool relevant = true;
            try
            {
                if (entry.definition.relevant)
                {
                    relevant = entry.definition.relevant();
                }
            }
            catch (...)
            {
                // DrawRelevant reports provider faults in the UI. A read-only
                // relevance query simply treats a failed provider as hidden.
                relevant = false;
            }

            if (relevant)
            {
                relevantIds.push_back(entry.definition.id);
            }
        }

        std::vector<const InspectorProviderDefinition*> result;
        result.reserve(relevantIds.size());
        for (const std::string& id : relevantIds)
        {
            const auto current = std::ranges::find_if(
                providers_,
                [&id](const Entry& entry)
                {
                    return entry.definition.id == id;
                });

            if (current != providers_.end())
            {
                result.push_back(&current->definition);
            }
        }
        return result;
    }

    [[nodiscard]] std::vector<std::string> DrawRelevant(
        editor_ui::PanelContext& context) const
    {
        // Draw from value snapshots. A provider is allowed to trigger plugin
        // reload/unload as a side effect of a command, which can mutate this
        // registry. Snapshotting avoids dangling pointers for the rest of the
        // current Inspector frame.
        std::vector<Entry> snapshot = providers_;
        std::ranges::stable_sort(
            snapshot,
            [](const Entry& left, const Entry& right)
            {
                if (left.definition.order != right.definition.order)
                {
                    return left.definition.order < right.definition.order;
                }
                return left.sequence < right.sequence;
            });

        std::vector<std::string> drawn;
        for (const Entry& entry : snapshot)
        {
            const InspectorProviderDefinition& provider =
                entry.definition;

            bool relevant = true;
            try
            {
                if (provider.relevant)
                {
                    relevant = provider.relevant();
                }
            }
            catch (const std::exception& exception)
            {
                context.ErrorText(
                    provider.title +
                    " relevance check failed: " +
                    exception.what());
                continue;
            }
            catch (...)
            {
                context.ErrorText(
                    provider.title +
                    " relevance check failed with an unknown error.");
                continue;
            }

            if (!relevant)
            {
                continue;
            }

            std::string section = provider.title;
            section += "##inspector-provider-";
            section += provider.id;

            if (!context.Section(section, provider.defaultOpen))
            {
                continue;
            }

            try
            {
                provider.draw(context);
                drawn.push_back(provider.id);
            }
            catch (const std::exception& exception)
            {
                context.ErrorText(
                    provider.title +
                    " failed: " +
                    exception.what());
            }
            catch (...)
            {
                context.ErrorText(
                    provider.title +
                    " failed with an unknown error.");
            }
        }
        return drawn;
    }

private:
    struct Entry
    {
        InspectorProviderDefinition definition;
        u64 sequence{0};
    };

    std::vector<Entry> providers_;
    u64 nextSequence_{0};
    u64 revision_{0};
};
} // namespace orbit::studio_ui
