#include <orbit/editor_session/ActiveBodyModel.hpp>

#include <orbit/universe/ReferenceSurface.hpp>

#include <stdexcept>
#include <utility>
#include <vector>

namespace orbit::editor_session
{
ActiveBodyModel::ActiveBodyModel(
    EditorWorldSession& session) noexcept
    : session_(&session)
{
}

bool ActiveBodyModel::Refresh()
{
    if (session_ == nullptr ||
        !session_->HasWorld())
    {
        const bool changed = active_.has_value();
        active_.reset();
        observedGeneration_ =
            session_ != nullptr
                ? session_->Generation()
                : 0;
        return changed;
    }

    const u64 generation =
        session_->Generation();
    const bool generationChanged =
        generation != observedGeneration_;

    if (generationChanged)
    {
        active_.reset();
        observedGeneration_ = generation;
    }

    const bool universeChanged =
        session_->RefreshUniverseIfChanged();

    std::optional<scene::ObjectId> targetObject;

    for (const auto selected :
         session_->Selection().Ordered())
    {
        targetObject = ResolveBodyObject(selected);

        if (targetObject.has_value())
        {
            break;
        }
    }

    if (!targetObject.has_value() &&
        active_.has_value())
    {
        const auto stillValid =
            session_->Universe().BodyForObject(
                active_->semanticObject);

        if (stillValid.has_value())
        {
            targetObject =
                active_->semanticObject;
        }
    }

    if (!targetObject.has_value())
    {
        targetObject = FirstBodyObject();
    }

    if (!targetObject.has_value())
    {
        const bool changed = active_.has_value();
        active_.reset();
        return changed ||
            generationChanged ||
            universeChanged;
    }

    ActiveBodyTarget next =
        BuildTarget(*targetObject);

    const bool changed =
        !active_.has_value() ||
        active_->semanticObject !=
            next.semanticObject ||
        active_->body != next.body ||
        active_->frame != next.frame ||
        active_->name != next.name ||
        active_->referenceRadiusMeters !=
            next.referenceRadiusMeters ||
        active_->sessionGeneration !=
            next.sessionGeneration ||
        active_->sourceRevision !=
            next.sourceRevision;

    active_ = std::move(next);
    return changed ||
        generationChanged ||
        universeChanged;
}

std::optional<ActiveBodyTarget>
ActiveBodyModel::Resolve(
    const scene::ObjectId object)
{
    if (session_ == nullptr ||
        !session_->HasWorld())
    {
        return std::nullopt;
    }

    observedGeneration_ =
        session_->Generation();
    static_cast<void>(
        session_->RefreshUniverseIfChanged());

    const auto bodyObject =
        ResolveBodyObject(object);

    if (!bodyObject.has_value())
    {
        return std::nullopt;
    }

    return BuildTarget(*bodyObject);
}

void ActiveBodyModel::Focus(
    const scene::ObjectId object)
{
    const auto target = Resolve(object);

    if (!target.has_value())
    {
        throw std::invalid_argument(
            "The requested semantic object is not part of a composed celestial body.");
    }

    active_ = *target;
}

void ActiveBodyModel::Clear() noexcept
{
    active_.reset();
}

const std::optional<ActiveBodyTarget>&
ActiveBodyModel::Active() const noexcept
{
    return active_;
}

std::optional<scene::ObjectId>
ActiveBodyModel::ResolveBodyObject(
    scene::ObjectId object) const
{
    if (session_ == nullptr ||
        !session_->HasWorld())
    {
        return std::nullopt;
    }

    while (object)
    {
        if (session_->Universe().
                BodyForObject(object).
                has_value())
        {
            return object;
        }

        const auto record =
            session_->Objects().Find(object);

        if (!record.has_value() ||
            !record->parent.has_value())
        {
            break;
        }

        object = *record->parent;
    }

    return std::nullopt;
}

std::optional<scene::ObjectId>
ActiveBodyModel::FirstBodyObject() const
{
    if (session_ == nullptr ||
        !session_->HasWorld())
    {
        return std::nullopt;
    }

    std::vector<scene::ObjectRecord> pending =
        session_->Objects().Roots();

    for (std::size_t index = 0;
         index < pending.size();
         ++index)
    {
        const auto& object = pending[index];

        if (session_->Universe().
                BodyForObject(object.id).
                has_value())
        {
            return object.id;
        }

        auto children =
            session_->Objects().Children(object.id);
        pending.insert(
            pending.end(),
            children.begin(),
            children.end());
    }

    return std::nullopt;
}

ActiveBodyTarget ActiveBodyModel::BuildTarget(
    const scene::ObjectId object) const
{
    const auto bodyId =
        session_->Universe().BodyForObject(object);

    if (!bodyId.has_value())
    {
        throw std::logic_error(
            "Active body semantic object is missing from UniverseComposition.");
    }

    const auto* body =
        session_->Universe().Bodies().
            FindBody(*bodyId);

    if (body == nullptr)
    {
        throw std::logic_error(
            "Active runtime body is missing from BodyRegistry.");
    }

    return {
        .semanticObject = object,
        .body = *bodyId,
        .frame = body->frame,
        .name = body->name,
        .referenceRadiusMeters =
            universe::ReferenceRadiusMeters(
                body->shape),
        .sessionGeneration =
            session_->Generation(),
        .sourceRevision =
            session_->Universe().
                SourceRevision()
    };
}
} // namespace orbit::editor_session
