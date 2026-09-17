#include <orbit/studio_session/UniverseBoundPathCache.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace orbit::studio_session
{
UniverseBoundPathCache::UniverseBoundPathCache(
    editor_session::EditorWorldSession& world) noexcept
    : world_(&world),
      observedUniverseGeneration_(
          world.UniverseGeneration()),
      bindingGeneration_(1)
{
}

bool UniverseBoundPathCache::RefreshBinding()
{
    if (world_ == nullptr)
    {
        return false;
    }

    const u64 currentGeneration =
        world_->UniverseGeneration();

    if (currentGeneration ==
        observedUniverseGeneration_)
    {
        return false;
    }

    Clear();
    observedUniverseGeneration_ =
        currentGeneration;
    ++bindingGeneration_;
    return true;
}

void UniverseBoundPathCache::Store(
    path_geometry::PathDerivedProduct product,
    const std::optional<u64> routeGeneration)
{
    static_cast<void>(RefreshBinding());

    if (world_ == nullptr ||
        !world_->HasWorld())
    {
        throw std::logic_error(
            "Cannot store derived path geometry without an open world.");
    }

    if (!product.edge)
    {
        throw std::invalid_argument(
            "Derived path geometry must reference a valid PathEdge object.");
    }

    if (!product.frame ||
        !world_->Universe().Frames().Contains(
            product.frame))
    {
        throw std::invalid_argument(
            "Derived path geometry frame does not belong to the active universe composition.");
    }

    const scene::ObjectId edge = product.edge;
    products_[edge] = std::move(product);

    if (routeGeneration.has_value())
    {
        routeGenerations_[edge] =
            *routeGeneration;
    }
    else
    {
        routeGenerations_.erase(edge);
    }
}

const path_geometry::PathDerivedProduct*
UniverseBoundPathCache::Find(
    const scene::ObjectId edge) const noexcept
{
    if (world_ == nullptr ||
        observedUniverseGeneration_ !=
            world_->UniverseGeneration())
    {
        return nullptr;
    }

    const auto found = products_.find(edge);
    return found == products_.end()
        ? nullptr
        : &found->second;
}

std::optional<u64>
UniverseBoundPathCache::RouteGeneration(
    const scene::ObjectId edge) const noexcept
{
    if (world_ == nullptr ||
        observedUniverseGeneration_ !=
            world_->UniverseGeneration())
    {
        return std::nullopt;
    }

    const auto found =
        routeGenerations_.find(edge);
    return found == routeGenerations_.end()
        ? std::nullopt
        : std::optional<u64>(found->second);
}

bool UniverseBoundPathCache::Erase(
    const scene::ObjectId edge) noexcept
{
    routeGenerations_.erase(edge);
    return products_.erase(edge) != 0U;
}

void UniverseBoundPathCache::Clear() noexcept
{
    products_.clear();
    routeGenerations_.clear();
}

std::size_t UniverseBoundPathCache::Size() const noexcept
{
    if (world_ == nullptr ||
        observedUniverseGeneration_ !=
            world_->UniverseGeneration())
    {
        return 0U;
    }

    return products_.size();
}

bool UniverseBoundPathCache::Empty() const noexcept
{
    return Size() == 0U;
}

std::vector<const path_geometry::PathDerivedProduct*>
UniverseBoundPathCache::Products() const
{
    std::vector<const path_geometry::PathDerivedProduct*> result;

    if (world_ == nullptr ||
        observedUniverseGeneration_ !=
            world_->UniverseGeneration())
    {
        return result;
    }

    result.reserve(products_.size());

    for (const auto& [edge, product] : products_)
    {
        static_cast<void>(edge);
        result.push_back(&product);
    }

    std::sort(
        result.begin(),
        result.end(),
        [](const auto* left,
           const auto* right)
        {
            if (left->edge.high != right->edge.high)
            {
                return left->edge.high < right->edge.high;
            }

            return left->edge.low < right->edge.low;
        });

    return result;
}

u64 UniverseBoundPathCache::ObservedUniverseGeneration() const noexcept
{
    return observedUniverseGeneration_;
}

u64 UniverseBoundPathCache::BindingGeneration() const noexcept
{
    return bindingGeneration_;
}
} // namespace orbit::studio_session
