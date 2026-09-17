#include <orbit/studio_session/ViewportTargetRegistry.hpp>

#include <stdexcept>
#include <utility>

namespace orbit::studio_session
{
ViewportTargetRegistry::ViewportTargetRegistry(
    editor_session::EditorWorldSession& world,
    editor_session::ActiveBodyModel& activeBody) noexcept
    : world_(&world),
      activeBody_(&activeBody)
{
}

void ViewportTargetRegistry::Register(
    std::string id,
    const ViewportMode mode,
    const bool followActiveBody)
{
    if (id.empty())
    {
        throw std::invalid_argument(
            "Viewport target ID must not be empty.");
    }

    auto [item, inserted] =
        views_.emplace(
            id,
            ViewportTargetState{
                .id = id,
                .mode = mode,
                .followActiveBody = followActiveBody
            });

    if (!inserted)
    {
        throw std::invalid_argument(
            "Viewport target ID is already registered.");
    }

    static_cast<void>(item);
    static_cast<void>(Refresh());
}

bool ViewportTargetRegistry::Unregister(
    const std::string_view id) noexcept
{
    return views_.erase(std::string(id)) > 0U;
}

void ViewportTargetRegistry::SetMode(
    const std::string_view id,
    const ViewportMode mode)
{
    Require(id).mode = mode;
}

void ViewportTargetRegistry::FollowActiveBody(
    const std::string_view id)
{
    auto& view = Require(id);
    view.followActiveBody = true;
    view.pinnedSemanticObject.reset();

    if (activeBody_ != nullptr)
    {
        view.target = activeBody_->Active();
    }
}

void ViewportTargetRegistry::PinToObject(
    const std::string_view id,
    const scene::ObjectId object)
{
    if (activeBody_ == nullptr)
    {
        throw std::logic_error(
            "Viewport target registry has no active-body resolver.");
    }

    const auto target = activeBody_->Resolve(object);

    if (!target.has_value())
    {
        throw std::invalid_argument(
            "Viewport target object is not part of a composed celestial body.");
    }

    auto& view = Require(id);
    view.followActiveBody = false;
    view.pinnedSemanticObject =
        target->semanticObject;
    view.target = target;
}

bool ViewportTargetRegistry::Refresh()
{
    if (world_ == nullptr ||
        activeBody_ == nullptr)
    {
        return false;
    }

    const u64 generation = world_->Generation();
    const bool generationChanged =
        generation != observedGeneration_;
    observedGeneration_ = generation;

    if (world_->HasWorld())
    {
        static_cast<void>(activeBody_->Refresh());
    }

    bool changed = generationChanged;

    for (auto& [id, view] : views_)
    {
        static_cast<void>(id);
        const auto previous = view.target;

        if (!world_->HasWorld())
        {
            view.target.reset();
        }
        else if (view.followActiveBody)
        {
            view.target = activeBody_->Active();
        }
        else if (view.pinnedSemanticObject.has_value())
        {
            view.target = activeBody_->Resolve(
                *view.pinnedSemanticObject);

            if (!view.target.has_value())
            {
                view.pinnedSemanticObject.reset();
            }
        }
        else
        {
            view.target.reset();
        }

        if (previous.has_value() !=
                view.target.has_value())
        {
            changed = true;
            continue;
        }

        if (previous.has_value() &&
            (previous->body != view.target->body ||
             previous->frame != view.target->frame ||
             previous->semanticObject !=
                 view.target->semanticObject ||
             previous->sourceRevision !=
                 view.target->sourceRevision ||
             previous->sessionGeneration !=
                 view.target->sessionGeneration))
        {
            changed = true;
        }
    }

    return changed;
}

const ViewportTargetState*
ViewportTargetRegistry::Find(
    const std::string_view id) const noexcept
{
    const auto found = views_.find(id);
    return found == views_.end()
        ? nullptr
        : &found->second;
}

std::vector<ViewportTargetState>
ViewportTargetRegistry::Catalog() const
{
    std::vector<ViewportTargetState> result;
    result.reserve(views_.size());

    for (const auto& [id, view] : views_)
    {
        static_cast<void>(id);
        result.push_back(view);
    }

    return result;
}

ViewportTargetState& ViewportTargetRegistry::Require(
    const std::string_view id)
{
    const auto found = views_.find(id);

    if (found == views_.end())
    {
        throw std::out_of_range(
            "Viewport target ID is not registered.");
    }

    return found->second;
}
} // namespace orbit::studio_session
