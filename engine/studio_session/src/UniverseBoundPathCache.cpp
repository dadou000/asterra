#include <orbit/studio_session/UniverseBoundPathCache.hpp>
#include <orbit/jobs/JobSystem.hpp>

#include <chrono>

#include <algorithm>
#include <orbit/paths/PathNetwork.hpp>
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

bool UniverseBoundPathCache::StoreCorridorIfChanged(
    const u64 authoredSignature,
    const path_geometry::PathCenterline& centerline,
    const paths::PathProfile& profile,
    const path_geometry::CorridorSweep& corridor,
    const path_geometry::PathBuildOptions options,
    const path_geometry::CorridorRefinementOptions refinement,
    const std::optional<u64> routeGeneration)
{
    static_cast<void>(RefreshBinding());
    if (authoredSignature == 0U)
        throw std::invalid_argument("Corridor authored signature must be nonzero.");
    const auto found = corridorAuthoredSignatures_.find(centerline.edge);
    if (found != corridorAuthoredSignatures_.end() &&
        found->second == authoredSignature && !IsDirty(centerline.edge) && Find(centerline.edge) != nullptr &&
        RouteGeneration(centerline.edge) == routeGeneration)
        return false;
    StoreCorridor(centerline, profile, corridor, options, refinement, routeGeneration);
    corridorAuthoredSignatures_[centerline.edge] = authoredSignature;
    dirtyCorridors_.erase(centerline.edge);
    return true;
}

void UniverseBoundPathCache::StoreCorridor(
    const path_geometry::PathCenterline& centerline,
    const paths::PathProfile& profile,
    const path_geometry::CorridorSweep& corridor,
    const path_geometry::PathBuildOptions options,
    const path_geometry::CorridorRefinementOptions refinement,
    const std::optional<u64> routeGeneration)
{
    // Build before replacing an existing cache product. A failed candidate
    // leaves the previous valid revision available for visualization.
    auto product = path_geometry::BuildCorridorDerived(
        centerline, profile, corridor, options, refinement);
    Store(std::move(product), routeGeneration);
    corridorAuthoredSignatures_.erase(centerline.edge);
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
    corridorAuthoredSignatures_.erase(edge);
    dirtyCorridors_.erase(edge);
    rebuildErrors_.erase(edge);
    products_[edge] = std::move(product);
    dependencySnapshots_[edge] = CaptureDependencies(edge);

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

UniverseBoundPathCache::DependencySnapshot
UniverseBoundPathCache::CaptureDependencies(const scene::ObjectId edge) const
{
    DependencySnapshot snapshot;
    if (world_ == nullptr || !world_->HasWorld()) return snapshot;
    const auto& objects = world_->Objects();
    const auto record = objects.Find(edge);
    if (!record.has_value() || record->type != paths::kPathEdgeType)
        return snapshot;
    const auto capture = [&](scene::ObjectId id, schema::PropertyId key) {
        snapshot.properties.push_back(objects.GetProperty(id, key));
    };
    // Stable graph topology, handles, profile, catalog and construction.
    for (const auto key : {paths::kEdgeStartNode, paths::kEdgeEndNode,
        paths::kEdgeMode, paths::kBezierStartHandle, paths::kBezierEndHandle,
        paths::kEdgeProfileOverride, paths::kInfrastructureCatalogId,
        paths::kInfrastructureCatalogVersion, paths::kInfrastructureConstruction,
        paths::kInfrastructureDesignSpeed, paths::kInfrastructureEaseTurns})
        capture(edge, key);
    const auto endpoint = [&](schema::PropertyId key) {
        const auto value = objects.GetProperty(edge, key);
        if (!value || !std::holds_alternative<schema::ObjectReferenceValue>(*value)) return;
        const auto id = std::get<schema::ObjectReferenceValue>(*value);
        const scene::ObjectId node{.high=id.high, .low=id.low};
        // Explicitly encode missing/deleted nodes in the snapshot.
        snapshot.properties.emplace_back(objects.Find(node).has_value());
        for (const auto property : {paths::kAnchorKind, paths::kAnchorFrame,
            paths::kAnchorPosition, paths::kAnchorBody, paths::kAnchorSurface,
            paths::kAnchorEntity, paths::kAnchorSocket})
            capture(node, property);
    };
    endpoint(paths::kEdgeStartNode);
    endpoint(paths::kEdgeEndNode);
    // Network profile changes also affect derived geometry.
    if (record->parent)
        capture(*record->parent, paths::kNetworkProfile);
    return snapshot;
}

std::size_t UniverseBoundPathCache::RefreshSemanticInvalidation()
{
    static_cast<void>(RefreshBinding());
    if (world_ == nullptr || !world_->HasWorld()) return 0;
    const u64 revision = world_->Objects().Revision();
    if (revision == observedSceneRevision_) return 0;
    observedSceneRevision_ = revision;
    std::size_t invalidated = 0;
    for (auto it = products_.begin(); it != products_.end();)
    {
        const auto edge = it->first;
        if (!world_->Objects().Find(edge).has_value())
        {
            routeGenerations_.erase(edge);
            dirtyTokens_.erase(edge);
            pendingCorridors_.erase(edge);
            corridorAuthoredSignatures_.erase(edge);
            dependencySnapshots_.erase(edge);
            dirtyCorridors_.erase(edge);
            it = products_.erase(it);
            ++invalidated;
            continue;
        }
        const auto current = CaptureDependencies(edge);
        const auto old = dependencySnapshots_.find(edge);
        if ((old == dependencySnapshots_.end() || old->second != current) &&
            !IsDirty(edge))
        {
            MarkDirty(edge);
            ++invalidated;
        }
        ++it;
    }
    return invalidated;
}

std::size_t UniverseBoundPathCache::RebuildDirty(
    const RebuildResolver& resolver, const std::size_t budget)
{
    static_cast<void>(RefreshBinding());
    if (!resolver || budget == 0 || world_ == nullptr || !world_->HasWorld())
        return 0;

    // Poll completed work without ever waiting for unfinished mesh generation.
    // packaged_task futures can be discarded safely on invalidation.
    for (auto it = pendingCorridors_.begin(); it != pendingCorridors_.end();)
    {
        if (it->second.result.wait_for(std::chrono::seconds(0)) !=
            std::future_status::ready)
        {
            ++it;
            continue;
        }
        const scene::ObjectId edge = it->first;
        auto& pending = it->second;
        const bool current =
            pending.bindingGeneration == bindingGeneration_ &&
            pending.dirtyToken == dirtyTokens_[edge] &&
            dirtyCorridors_.contains(edge) &&
            pending.sourceDependencies == CaptureDependencies(edge);
        if (current)
        {
            try
            {
                auto product = pending.result.get();
                Store(std::move(product), pending.routeGeneration);
                corridorAuthoredSignatures_[edge] = pending.authoredSignature;
            }
            catch (const std::exception& error)
            {
                rebuildErrors_[edge] = error.what();
            }
        }
        it = pendingCorridors_.erase(it);
    }

    // Avoid unbounded allocation and background backlog during rapid editing.
    constexpr std::size_t kMaxPendingCorridors = 8U;
    if (pendingCorridors_.size() >= kMaxPendingCorridors)
        return 0;

    std::vector<scene::ObjectId> candidates;
    for (const auto edge : dirtyCorridors_)
        if (!rebuildErrors_.contains(edge) && !pendingCorridors_.contains(edge))
            candidates.push_back(edge);
    std::sort(candidates.begin(), candidates.end(), [](const auto a, const auto b) {
        return a.high != b.high ? a.high < b.high : a.low < b.low;
    });

    std::size_t scheduled = 0;
    for (const auto edge : candidates)
    {
        if (scheduled >= budget || pendingCorridors_.size() >= kMaxPendingCorridors)
            break;
        try
        {
            auto request = resolver(edge);
            if (!request)
            {
                rebuildErrors_[edge] = "No current corridor build request for edge.";
                ++scheduled;
                continue;
            }
            if (request->centerline.edge != edge)
                throw std::invalid_argument("Rebuild request edge does not match dirty edge.");

            PendingCorridor pending{};
            pending.authoredSignature = request->authoredSignature;
            pending.routeGeneration = request->routeGeneration;
            pending.bindingGeneration = bindingGeneration_;
            pending.dirtyToken = dirtyTokens_[edge];
            pending.sourceDependencies = CaptureDependencies(edge);
            pending.result = jobs::SubmitBackgroundCpu(
                [snapshot = std::move(*request)]() mutable {
                    return path_geometry::BuildCorridorDerived(
                        snapshot.centerline, snapshot.profile,
                        snapshot.corridor, snapshot.options,
                        snapshot.refinement);
                }, jobs::JobPriority::Normal);
            pendingCorridors_.emplace(edge, std::move(pending));
            ++scheduled;
        }
        catch (const std::exception& error)
        {
            rebuildErrors_[edge] = error.what();
            ++scheduled;
        }
    }
    return scheduled;
}

std::string UniverseBoundPathCache::RebuildError(const scene::ObjectId edge) const
{
    const auto it = rebuildErrors_.find(edge);
    return it == rebuildErrors_.end() ? std::string{} : it->second;
}

std::size_t UniverseBoundPathCache::DirtyCount() const noexcept
{
    return dirtyCorridors_.size();
}

void UniverseBoundPathCache::MarkDirty(const scene::ObjectId edge) noexcept
{
    if (edge)
    {
        dirtyCorridors_.insert(edge);
        rebuildErrors_.erase(edge);
        dirtyTokens_[edge] = ++nextDirtyToken_;
        // Never destroy a std::async future here: background results are
        // packaged tasks, so dropping an invalidated result cannot join.
        pendingCorridors_.erase(edge);
    }
}

bool UniverseBoundPathCache::IsDirty(const scene::ObjectId edge) const noexcept
{
    return dirtyCorridors_.contains(edge);
}

bool UniverseBoundPathCache::Erase(
    const scene::ObjectId edge) noexcept
{
    routeGenerations_.erase(edge);
    dirtyCorridors_.erase(edge);
    dirtyTokens_.erase(edge);
    pendingCorridors_.erase(edge);
    rebuildErrors_.erase(edge);
    corridorAuthoredSignatures_.erase(edge);
    dependencySnapshots_.erase(edge);
    return products_.erase(edge) != 0U;
}

void UniverseBoundPathCache::Clear() noexcept
{
    products_.clear();
    pendingCorridors_.clear();
    dirtyTokens_.clear();
    routeGenerations_.clear();
    corridorAuthoredSignatures_.clear();
    dependencySnapshots_.clear();
    observedSceneRevision_ = ~u64{0};
    dirtyCorridors_.clear();
    rebuildErrors_.clear();
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
