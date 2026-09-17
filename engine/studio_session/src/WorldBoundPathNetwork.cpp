#include <orbit/studio_session/WorldBoundPathNetwork.hpp>

#include <stdexcept>

namespace orbit::studio_session
{
WorldBoundPathNetwork::WorldBoundPathNetwork(
    editor_session::EditorWorldSession& world) noexcept
    : world_(&world)
{
}

bool WorldBoundPathNetwork::RefreshBinding()
{
    if (world_ == nullptr)
    {
        return false;
    }

    const u64 generation = world_->Generation();

    if (generation == observedWorldGeneration_)
    {
        return false;
    }

    service_.reset();
    observedWorldGeneration_ = generation;
    ++bindingGeneration_;

    if (world_->HasWorld())
    {
        service_ =
            std::make_unique<paths::PathNetworkService>(
                world_->Objects(),
                world_->Commands());
    }

    return true;
}

bool WorldBoundPathNetwork::HasService() const noexcept
{
    return service_ != nullptr;
}

paths::PathNetworkService& WorldBoundPathNetwork::Service()
{
    static_cast<void>(RefreshBinding());

    if (service_ == nullptr)
    {
        throw std::logic_error(
            "No authoring world is open for the Studio path network.");
    }

    return *service_;
}

const paths::PathNetworkService&
WorldBoundPathNetwork::Service() const
{
    return const_cast<WorldBoundPathNetwork*>(this)->Service();
}

u64 WorldBoundPathNetwork::ObservedWorldGeneration() const noexcept
{
    return observedWorldGeneration_;
}

u64 WorldBoundPathNetwork::BindingGeneration() const noexcept
{
    return bindingGeneration_;
}
} // namespace orbit::studio_session
