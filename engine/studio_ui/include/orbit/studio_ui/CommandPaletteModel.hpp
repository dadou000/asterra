#pragma once

#include <orbit/commands/CommandRegistry.hpp>

#include <algorithm>
#include <cctype>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orbit::studio_ui
{
struct CommandPaletteEntry
{
    commands::CommandId command{};
    std::string label;
    std::string category;
    std::string description;
    bool requiresArguments{false};
    i32 score{0};
};

[[nodiscard]] inline std::string PaletteLower(
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

[[nodiscard]] inline i32 PaletteMatchScore(
    const std::string_view source,
    const std::string_view query)
{
    if (query.empty())
    {
        return 1;
    }

    const std::string lowerSource = PaletteLower(source);
    const std::string lowerQuery = PaletteLower(query);

    if (lowerSource == lowerQuery)
    {
        return 10'000;
    }

    if (lowerSource.starts_with(lowerQuery))
    {
        return 8'000 -
            static_cast<i32>(lowerSource.size() - lowerQuery.size());
    }

    if (const auto position = lowerSource.find(lowerQuery);
        position != std::string::npos)
    {
        return 6'000 - static_cast<i32>(position);
    }

    std::size_t cursor = 0;
    i32 gapPenalty = 0;
    for (const char value : lowerQuery)
    {
        const auto found = lowerSource.find(value, cursor);
        if (found == std::string::npos)
        {
            return std::numeric_limits<i32>::min();
        }

        gapPenalty += static_cast<i32>(found - cursor);
        cursor = found + 1U;
    }

    return 3'000 - gapPenalty;
}

[[nodiscard]] inline std::vector<CommandPaletteEntry>
BuildCommandPalette(
    const std::vector<commands::CommandCatalogEntry>& catalog)
{
    std::vector<CommandPaletteEntry> result;
    result.reserve(catalog.size());

    for (const auto& command : catalog)
    {
        result.push_back({
            .command = command.id,
            .label = command.name,
            .category = command.category,
            .description = command.description,
            .requiresArguments = std::ranges::any_of(
                command.parameters,
                [](const commands::CommandParameter& parameter)
                {
                    return parameter.required;
                })
        });
    }

    std::ranges::stable_sort(
        result,
        [](const CommandPaletteEntry& left,
           const CommandPaletteEntry& right)
        {
            if (left.category != right.category)
            {
                return left.category < right.category;
            }
            return left.label < right.label;
        });

    return result;
}

[[nodiscard]] inline std::vector<CommandPaletteEntry>
SearchCommandPalette(
    const std::vector<CommandPaletteEntry>& entries,
    const std::string_view query,
    const std::size_t limit = 32U)
{
    std::vector<CommandPaletteEntry> result;
    result.reserve(std::min(limit, entries.size()));

    for (const auto& entry : entries)
    {
        const i32 labelScore =
            PaletteMatchScore(entry.label, query);
        const i32 categoryScore =
            PaletteMatchScore(entry.category, query);
        const i32 descriptionScore =
            PaletteMatchScore(entry.description, query);

        const i32 score = std::max({
            labelScore,
            categoryScore > std::numeric_limits<i32>::min()
                ? categoryScore - 1'000
                : categoryScore,
            descriptionScore > std::numeric_limits<i32>::min()
                ? descriptionScore - 2'000
                : descriptionScore
        });

        if (!query.empty() &&
            score == std::numeric_limits<i32>::min())
        {
            continue;
        }

        auto copy = entry;
        copy.score = score;
        result.push_back(std::move(copy));
    }

    std::ranges::stable_sort(
        result,
        [](const CommandPaletteEntry& left,
           const CommandPaletteEntry& right)
        {
            if (left.score != right.score)
            {
                return left.score > right.score;
            }
            if (left.category != right.category)
            {
                return left.category < right.category;
            }
            return left.label < right.label;
        });

    if (result.size() > limit)
    {
        result.resize(limit);
    }

    return result;
}
} // namespace orbit::studio_ui
