#include <orbit/studio_session/StudioRuntimeBinding.hpp>

#include <orbit/studio_session/StudioSession.hpp>

#include <stdexcept>

namespace orbit::studio_session
{
StudioRuntimeBinding::StudioRuntimeBinding(
    StudioSession& session) noexcept
    : session_(&session)
{
}

StudioRuntimeSnapshot StudioRuntimeBinding::Refresh()
{
    if (session_ == nullptr)
    {
        return {};
    }

    const StudioTickResult tick =
        session_->Tick();

    StudioRuntimeSnapshot snapshot{
        .hasWorld = session_->World().HasWorld(),
        .worldGeneration = tick.worldGeneration,
        .universeGeneration = tick.universeGeneration,
        .compositionChanged = tick.compositionChanged,
        .activeBodyChanged = tick.activeBodyChanged,
        .viewportTargetsChanged = tick.viewportTargetsChanged,
        .terrainRuntimeChanged = tick.terrainRuntimeChanged,
        .pathNetworkRebound = tick.pathNetworkRebound,
        .pathRoutingRebound = tick.pathRoutingRebound,
        .pathProductsInvalidated = tick.pathProductsInvalidated,
        .pluginsReloaded = tick.pluginsReloaded
    };

    if (session_->ActiveBody().Active().has_value())
    {
        snapshot.activeBody =
            *session_->ActiveBody().Active();
    }

    return snapshot;
}

StudioRuntimeSnapshot StudioRuntimeBinding::Capture() const
{
    if (session_ == nullptr)
    {
        return {};
    }

    StudioRuntimeSnapshot snapshot{
        .hasWorld = session_->World().HasWorld(),
        .worldGeneration = session_->World().Generation(),
        .universeGeneration =
            session_->World().UniverseGeneration()
    };

    if (session_->ActiveBody().Active().has_value())
    {
        snapshot.activeBody =
            *session_->ActiveBody().Active();
    }

    return snapshot;
}

bool StudioRuntimeBinding::IsCurrent(
    const StudioRuntimeSnapshot& snapshot) const noexcept
{
    if (session_ == nullptr)
    {
        return false;
    }

    const auto& world = session_->World();

    if (snapshot.hasWorld != world.HasWorld() ||
        snapshot.worldGeneration != world.Generation() ||
        snapshot.universeGeneration != world.UniverseGeneration())
    {
        return false;
    }

    if (snapshot.activeBody.has_value())
    {
        if (!snapshot.hasWorld ||
            snapshot.activeBody->sessionGeneration !=
                snapshot.worldGeneration ||
            snapshot.activeBody->universeGeneration !=
                snapshot.universeGeneration)
        {
            return false;
        }
    }

    return true;
}

const frames::FrameGraph& StudioRuntimeBinding::Frames(
    const StudioRuntimeSnapshot& snapshot) const
{
    RequireCurrent(snapshot);

    if (!snapshot.hasWorld)
    {
        throw std::logic_error(
            "Studio runtime snapshot has no open world.");
    }

    return session_->World().Universe().Frames();
}

const universe::BodyRegistry& StudioRuntimeBinding::Bodies(
    const StudioRuntimeSnapshot& snapshot) const
{
    RequireCurrent(snapshot);

    if (!snapshot.hasWorld)
    {
        throw std::logic_error(
            "Studio runtime snapshot has no open world.");
    }

    return session_->World().Universe().Bodies();
}

std::optional<universe::CelestialBody>
StudioRuntimeBinding::ActiveBodyRecord(
    const StudioRuntimeSnapshot& snapshot) const
{
    RequireCurrent(snapshot);

    if (!snapshot.activeBody.has_value())
    {
        return std::nullopt;
    }

    const auto* body =
        session_->World().Universe().Bodies().FindBody(
            snapshot.activeBody->body);

    if (body == nullptr)
    {
        throw std::logic_error(
            "Current Studio active-body target is missing from BodyRegistry.");
    }

    return *body;
}

path_routing::RoutePlanner& StudioRuntimeBinding::Routes(
    const StudioRuntimeSnapshot& snapshot)
{
    RequireCurrent(snapshot);

    if (!snapshot.hasWorld)
    {
        throw std::logic_error(
            "Studio runtime snapshot has no open world.");
    }

    return session_->PathRouting().Planner();
}

UniverseBoundPathCache& StudioRuntimeBinding::PathProducts(
    const StudioRuntimeSnapshot& snapshot)
{
    RequireCurrent(snapshot);
    return session_->PathProducts();
}

const UniverseBoundPathCache& StudioRuntimeBinding::PathProducts(
    const StudioRuntimeSnapshot& snapshot) const
{
    RequireCurrent(snapshot);
    return session_->PathProducts();
}

void StudioRuntimeBinding::RequireCurrent(
    const StudioRuntimeSnapshot& snapshot) const
{
    if (!IsCurrent(snapshot))
    {
        throw std::logic_error(
            "Studio runtime snapshot is stale; refresh before accessing composed runtime state.");
    }
}
} // namespace orbit::studio_session
