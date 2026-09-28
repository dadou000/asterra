#pragma once

#include <orbit/scene/ObjectStore.hpp>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

namespace orbit::studio_ui
{
struct SelectionBreadcrumb
{
    scene::ObjectId id{};
    std::string label;
    schema::TypeId type{};
};

[[nodiscard]] inline std::vector<SelectionBreadcrumb>
BuildSelectionBreadcrumbs(
    const scene::ObjectStore& objects,
    const scene::ObjectId selected,
    const std::size_t maxDepth = 64U)
{
    std::vector<SelectionBreadcrumb> reversed;
    std::unordered_set<scene::ObjectId> visited;

    scene::ObjectId current = selected;
    for (std::size_t depth = 0;
         current.IsValid() && depth < maxDepth;
         ++depth)
    {
        if (!visited.insert(current).second)
        {
            break;
        }

        const auto record = objects.Find(current);
        if (!record.has_value())
        {
            break;
        }

        reversed.push_back({
            .id = record->id,
            .label = record->name,
            .type = record->type
        });

        if (!record->parent.has_value())
        {
            break;
        }
        current = *record->parent;
    }

    std::ranges::reverse(reversed);
    return reversed;
}
} // namespace orbit::studio_ui
