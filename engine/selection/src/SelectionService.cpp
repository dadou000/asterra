#include <orbit/selection/SelectionService.hpp>

#include <algorithm>

namespace orbit::selection
{
void SelectionService::Set(
    const std::span<
        const scene::ObjectId> objects)
{
    std::vector<scene::ObjectId> next;
    next.reserve(objects.size());

    for (const scene::ObjectId object :
         objects)
    {
        if (!object)
        {
            continue;
        }

        if (std::find(
                next.begin(),
                next.end(),
                object) ==
            next.end())
        {
            next.push_back(object);
        }
    }

    if (next == selected_)
    {
        return;
    }

    selected_ = std::move(next);
    ++revision_;
}

void SelectionService::Clear() noexcept
{
    if (selected_.empty())
    {
        return;
    }

    selected_.clear();
    ++revision_;
}

void SelectionService::Toggle(
    const scene::ObjectId object)
{
    if (!object)
    {
        return;
    }

    const auto found =
        std::find(
            selected_.begin(),
            selected_.end(),
            object);

    if (found == selected_.end())
    {
        selected_.push_back(object);
    }
    else
    {
        selected_.erase(found);
    }

    ++revision_;
}

bool SelectionService::Contains(
    const scene::ObjectId object) const noexcept
{
    return std::find(
               selected_.begin(),
               selected_.end(),
               object) !=
        selected_.end();
}

const std::vector<scene::ObjectId>&
SelectionService::Ordered() const noexcept
{
    return selected_;
}

u64 SelectionService::Revision() const noexcept
{
    return revision_;
}
} // namespace orbit::selection
